/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/util.h"

#include <boost/filesystem.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/config.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

//boost::filesystem::path getInterimFile中使用
//FTDCFileWriter._interimFile成员
const char kFTDCInterimFile[] = "metrics.interim";
//FTDCFileWriter._interimTempFile成员
const char kFTDCInterimTempFile[] = "metrics.interim.temp";
//参考FTDCFileManager::generateArchiveFileName，生成类似metrics.2021-11-09T08-10-30Z-00000得文件名
const char kFTDCArchiveFile[] = "metrics";

const char kFTDCIdField[] = "_id";
const char kFTDCTypeField[] = "type";

const char kFTDCDataField[] = "data";
const char kFTDCDocField[] = "doc";

const char kFTDCDocsField[] = "docs";

const char kFTDCCollectStartField[] = "start";
const char kFTDCCollectEndField[] = "end";

const std::int64_t FTDCConfig::kPeriodMillisDefault = 1000;

const std::size_t kMaxRecursion = 10;

namespace FTDCUtil {

namespace {
boost::filesystem::path appendFileName(const boost::filesystem::path& file, const char* filename) {
    if (boost::filesystem::is_directory(file)) {
        return file / filename;
    }

    auto p = file.parent_path();
    p /= filename;

    return p;
}
}  // namespace

//FTDCFileWriter::open   FTDCFileManager::recoverInterimFile()
boost::filesystem::path getInterimFile(const boost::filesystem::path& file) {
    return appendFileName(file, kFTDCInterimFile);
}

boost::filesystem::path getInterimTempFile(const boost::filesystem::path& file) {
    return appendFileName(file, kFTDCInterimTempFile);
}

Date_t roundTime(Date_t now, Milliseconds period) {
    // Note: auto type deduction is explicitly avoided here to ensure rigid type correctness
    long long clock_duration = now.toMillisSinceEpoch();

    long long now_next_period = clock_duration + period.count();

    long long excess_time(now_next_period % period.count());

    long long next_time = now_next_period - excess_time;

    return Date_t::fromMillisSinceEpoch(next_time);
}

boost::filesystem::path getMongoSPath(const boost::filesystem::path& logFile) {
    auto base = logFile;

    // Keep stripping file extensions until we are only left with the file name
    while (base.has_extension()) {
        auto full_path = base.generic_string();
        base = full_path.substr(0, full_path.size() - base.extension().size());
    }

    base += "." + kFTDCDefaultDirectory.toString();
    return base;
}

}  // namespace FTDCUtil


namespace FTDCBSONUtil {

namespace {

/**
 * Iterate a BSONObj but only return fields that have types that FTDC cares about.
 */
class FTDCBSONObjIterator {
public:
    FTDCBSONObjIterator(const BSONObj& obj) : _iterator(obj) {
        advance();
    }

    bool more() {
        return !_current.eoo();
    }

    BSONElement next() {
        auto ret = _current;
        advance();
        return ret;
    }

private:
    /**
     * Find the next element that is a valid FTDC type.
     */
    void advance() {
        _current = BSONElement();

        while (_iterator.more()) {

            auto elem = _iterator.next();
            if (isFTDCType(elem.type())) {
                _current = elem;
                break;
            }
        }
    }

private:
    BSONObjIterator _iterator;
    BSONElement _current;
};

//比较两个定时周期内的诊断项内容是否完全一样，一样返回true，同时存储解析的自动内容到metrics
//FTDCCompressor::addSample
StatusWith<bool> extractMetricsFromDocument(const BSONObj& referenceDoc,
                                            const BSONObj& currentDoc,
                                            std::vector<std::uint64_t>* metrics,
                                            bool matches,
                                            size_t recursion) {
    if (recursion > kMaxRecursion) { //统计项里面可能会有嵌套   如果零时修改diagnose内存的一些配置，可能本次和上次的诊断项内容不一致
        return {ErrorCodes::BadValue, "Recursion limit reached."};
    }

    FTDCBSONObjIterator itCurrent(currentDoc);
    FTDCBSONObjIterator itReference(referenceDoc);

    while (itCurrent.more()) {
        // Schema mismatch if current document is longer than reference document
        if (matches && !itReference.more()) {
            LOGV2_DEBUG(20633,
                        4,
                        "full-time diagnostic data capture schema change: current document is "
                        "longer than reference document");
            matches = false;
        }

        BSONElement currentElement = itCurrent.next();
        BSONElement referenceElement = matches ? itReference.next() : BSONElement();

        if (matches) {
			//例如 db.adminCommand( { setParameter: 1, diagnosticDataCollectionEnableLatencyHistograms:true} )修改参数
			//{"t":{"$date":"2021-12-25T17:19:01.002+08:00"},"s":"D4", "c":"FTDC",     "id":20634,   "ctx":"ftdc","msg":"full-time diagnostic data capture schema change: field name change","attr":{"from":"latency","to":"histogram"}}
			//{"t":{"$date":"2021-12-25T17:19:01.002+08:00"},"s":"D4", "c":"FTDC",     "id":20635,   "ctx":"ftdc","msg":"full-time diagnostic data capture schema change: field type change","attr":{"fieldName":"latency","oldType":18,"newType":4}}
            // Check for matching field names
            if (referenceElement.fieldNameStringData() != currentElement.fieldNameStringData()) {
                LOGV2_DEBUG(20634,
                            4,
                            "full-time diagnostic data capture schema change: field name change",
                            "from"_attr = referenceElement.fieldNameStringData(),
                            "to"_attr = currentElement.fieldNameStringData());
                matches = false;
            }

            // Check that types match, allowing any numeric type to match any other numeric type.
            // This looseness is necessary because some metrics use varying numeric types,
            // and if that was considered a schema mismatch, it would increase the number of
            // reference samples required.
            if ((currentElement.type() != referenceElement.type()) &&
                !(referenceElement.isNumber() == true &&
                  currentElement.isNumber() == referenceElement.isNumber())) {
                LOGV2_DEBUG(20635,
                            4,
                            "full-time diagnostic data capture schema change: field type change",
                            "fieldName"_attr = referenceElement.fieldNameStringData(),
                            "oldType"_attr = static_cast<int>(referenceElement.type()),
                            "newType"_attr = static_cast<int>(currentElement.type()));
                matches = false;
            }
        }

        switch (currentElement.type()) {
            // all numeric types are extracted as long (int64)
            // this supports the loose schema matching mentioned above,
            // but does create a range issue for doubles, and requires doubles to be integer
            case NumberDouble:
            case NumberInt:
            case NumberLong:
            case NumberDecimal:
                metrics->emplace_back(currentElement.numberLong());
                break;

            case Bool:
                metrics->emplace_back(currentElement.Bool());
                break;

            case Date:
                metrics->emplace_back(currentElement.Date().toMillisSinceEpoch());
                break;

            case bsonTimestamp:
                // very slightly more space efficient to treat these as two separate metrics
                metrics->emplace_back(currentElement.timestamp().getSecs());
                metrics->emplace_back(currentElement.timestamp().getInc());
                break;

            case Object:
            case Array: {
                // Maximum recursion is controlled by the documents we collect. Maximum is 5 in the
                // current implementation.
                auto sw = extractMetricsFromDocument(matches ? referenceElement.Obj() : BSONObj(),
                                                     currentElement.Obj(),
                                                     metrics,
                                                     matches,
                                                     recursion + 1);
                if (!sw.isOK()) {
                    return sw;
                }
                matches = matches && sw.getValue();
            } break;

            default:
                break;
        }
    }

    // schema mismatch if ref is longer than curr
    if (matches && itReference.more()) {
        LOGV2_DEBUG(20636,
                    4,
                    "full-time diagnostic data capture schema change: reference document is longer "
                    "than current");
        matches = false;
    }

    return {matches};
}

}  // namespace

bool isFTDCType(BSONType type) {
    switch (type) {
        case NumberDouble:
        case NumberInt:
        case NumberLong:
        case NumberDecimal:
        case Bool:
        case Date:
        case bsonTimestamp:
        case Object:
        case Array:
            return true;

        default:
            return false;
    }
}

StatusWith<bool> extractMetricsFromDocument(const BSONObj& referenceDoc,
                                            const BSONObj& currentDoc,
                                            std::vector<std::uint64_t>* metrics) {
    return extractMetricsFromDocument(referenceDoc, currentDoc, metrics, true, 0);
}

//FTDCDecompressor::uncompress
namespace {
Status constructDocumentFromMetrics(const BSONObj& referenceDocument,
                                    BSONObjBuilder& builder,
                                    const std::vector<std::uint64_t>& metrics,
                                    size_t* pos,
                                    size_t recursion) {
    if (recursion > kMaxRecursion) {
        return {ErrorCodes::BadValue, "Recursion limit reached."};
    }

    BSONObjIterator iterator(referenceDocument);
    while (iterator.more()) {
        BSONElement currentElement = iterator.next();

        switch (currentElement.type()) {
            case NumberDouble:
            case NumberInt:
            case NumberLong:
            case NumberDecimal:
                if (*pos >= metrics.size()) {
                    return Status(
                        ErrorCodes::BadValue,
                        "There are more metrics in the reference document then expected.");
                }

                builder.append(currentElement.fieldName(),
                               static_cast<long long int>(metrics[(*pos)++]));
                break;

            case Bool:
                if (*pos >= metrics.size()) {
                    return Status(
                        ErrorCodes::BadValue,
                        "There are more metrics in the reference document then expected.");
                }

                builder.append(currentElement.fieldName(), static_cast<bool>(metrics[(*pos)++]));
                break;

            case Date:
                if (*pos >= metrics.size()) {
                    return Status(
                        ErrorCodes::BadValue,
                        "There are more metrics in the reference document then expected.");
                }

                builder.append(
                    currentElement.fieldName(),
                    Date_t::fromMillisSinceEpoch(static_cast<std::uint64_t>(metrics[(*pos)++])));
                break;

            case bsonTimestamp: {
                if (*pos + 1 >= metrics.size()) {
                    return Status(
                        ErrorCodes::BadValue,
                        "There are more metrics in the reference document then expected.");
                }

                std::uint64_t seconds = metrics[(*pos)++];
                std::uint64_t increment = metrics[(*pos)++];
                builder.append(currentElement.fieldName(), Timestamp(seconds, increment));
                break;
            }

            case Object: {
                BSONObjBuilder sub(builder.subobjStart(currentElement.fieldName()));
                auto s = constructDocumentFromMetrics(
                    currentElement.Obj(), sub, metrics, pos, recursion + 1);
                if (!s.isOK()) {
                    return s;
                }
                break;
            }

            case Array: {
                BSONObjBuilder sub(builder.subarrayStart(currentElement.fieldName()));
                auto s = constructDocumentFromMetrics(
                    currentElement.Obj(), sub, metrics, pos, recursion + 1);
                if (!s.isOK()) {
                    return s;
                }
                break;
            }

            default:
                builder.append(currentElement);
                break;
        }
    }

    return Status::OK();
}

}  // namespace

StatusWith<BSONObj> constructDocumentFromMetrics(const BSONObj& ref,
                                                 const std::vector<std::uint64_t>& metrics) {
    size_t at = 0;
    BSONObjBuilder b;
    Status s = constructDocumentFromMetrics(ref, b, metrics, &at, 0);
    if (!s.isOK()) {
        return StatusWith<BSONObj>(s);
    }

    return b.obj();
}

//FTDCFileWriter::writeMetadata
//{"_id":{"$date":"2021-11-26T06:08:54.009Z"},"type":0,"doc":{xxx}  元数据信息，也就是metrics解析后的第一条json数据
BSONObj createBSONMetadataDocument(const BSONObj& metadata, Date_t date) {
    BSONObjBuilder builder;
    builder.appendDate(kFTDCIdField, date);
    builder.appendNumber(kFTDCTypeField, static_cast<int>(FTDCType::kMetadata));
    builder.appendObject(kFTDCDocField, metadata.objdata(), metadata.objsize());

    return builder.obj();
}

//FTDCFileWriter::writeSample    FTDCFileWriter::flush
//{"_id":{"$date":"2021-11-26T06:08:54.009Z"},"type":1,"data":{"$binary":"xxxx"}   xxxx是加密的诊断项信息
//诊断数据信息 _id+doc+
BSONObj createBSONMetricChunkDocument(ConstDataRange buf, Date_t date) {
    BSONObjBuilder builder;

    builder.appendDate(kFTDCIdField, date);
    builder.appendNumber(kFTDCTypeField, static_cast<int>(FTDCType::kMetricChunk));
    builder.appendBinData(kFTDCDataField, buf.length(), BinDataType::BinDataGeneral, buf.data());

    return builder.obj();
}

StatusWith<Date_t> getBSONDocumentId(const BSONObj& obj) {
    BSONElement element;

    Status status = bsonExtractTypedField(obj, kFTDCIdField, BSONType::Date, &element);
    if (!status.isOK()) {
        return {status};
    }

    return {element.Date()};
}

StatusWith<FTDCType> getBSONDocumentType(const BSONObj& obj) {
    long long value;

    Status status = bsonExtractIntegerField(obj, kFTDCTypeField, &value);
    if (!status.isOK()) {
        return {status};
    }

    if (static_cast<FTDCType>(value) != FTDCType::kMetricChunk &&
        static_cast<FTDCType>(value) != FTDCType::kMetadata) {
        return {ErrorCodes::BadValue,
                str::stream() << "Field '" << std::string(kFTDCTypeField)
                              << "' is not an expected value, found '" << value << "'"};
    }

    return {static_cast<FTDCType>(value)};
}

StatusWith<BSONObj> getBSONDocumentFromMetadataDoc(const BSONObj& obj) {
    if (kDebugBuild) {
        auto swType = getBSONDocumentType(obj);
        dassert(swType.isOK() && swType.getValue() == FTDCType::kMetadata);
    }

    BSONElement element;

    Status status = bsonExtractTypedField(obj, kFTDCDocField, BSONType::Object, &element);
    if (!status.isOK()) {
        return {status};
    }

    return {element.Obj()};
}

//FTDCFileReader::hasNext()
StatusWith<std::vector<BSONObj>> getMetricsFromMetricDoc(const BSONObj& obj,
                                                         FTDCDecompressor* decompressor) {
    if (kDebugBuild) {
        auto swType = getBSONDocumentType(obj);
        dassert(swType.isOK() && swType.getValue() == FTDCType::kMetricChunk);
    }

    BSONElement element;

    Status status = bsonExtractTypedField(obj, kFTDCDataField, BSONType::BinData, &element);
    if (!status.isOK()) {
        return {status};
    }

    int length;
    const char* buffer = element.binData(length);
    if (length < 0) {
        return {ErrorCodes::BadValue,
                str::stream() << "Field " << std::string(kFTDCTypeField) << " is not a BinData."};
    }

	//FTDCDecompressor::uncompress
    return decompressor->uncompress({buffer, static_cast<std::size_t>(length)});
}

}  // namespace FTDCBSONUtil

}  // namespace mongo
