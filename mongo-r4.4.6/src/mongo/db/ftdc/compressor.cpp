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
#include "mongo/logv2/log.h"

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/compressor.h"

#include "mongo/base/data_builder.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/ftdc/varint.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::swap;


/**
 * Add a bson document containing metrics into the compressor.
 *
 * Returns flag indicating whether the caller should flush the compressor buffer to disk.
 *	1. kCompressorFull if the compressor is considered full.
 *	2. kSchemaChanged if there was a schema change, and buffer should be flushed.
 *	3. kHasSpace if it has room for more metrics in the current buffer.
 *
 * date is the date at which the sample as started to be captured. It will be saved in the
 * compressor if this sample is used as the reference document.N
 */
//FTDCFileWriter::writeSample
StatusWith<boost::optional<std::tuple<ConstDataRange, FTDCCompressor::CompressorState, Date_t>>>
FTDCCompressor::addSample(const BSONObj& sample, Date_t date) {
    if (_referenceDoc.isEmpty()) {
		//第一次启动的时候走这里，解析除诊断项中的成员信息_metrics
        auto swMatchesReference =
            FTDCBSONUtil::extractMetricsFromDocument(sample, sample, &_metrics);
        if (!swMatchesReference.isOK()) {
            return swMatchesReference.getStatus();
        }

        _reset(sample, date);
        return {boost::none};
    }

	//上一次获取的诊断信息全部在_metrics中
    _metrics.resize(0);

    auto swMatches = FTDCBSONUtil::extractMetricsFromDocument(_referenceDoc, sample, &_metrics);

    if (!swMatches.isOK()) {//异常处理，例如嵌套递归太多
        return swMatches.getStatus();
    }

	//schema发生了变化，或者schema没有发生变化
    dassert((swMatches.getValue() == false || _metricsCount == _metrics.size()) &&
            _metrics.size() < std::numeric_limits<std::uint32_t>::max());

    // We need to flush the current set of samples since the BSON schema has changed.
    //例如配置发生变化，则内容schema就不一样了，这时候先把上一次的内容刷盘
    if (!swMatches.getValue()) {
        auto swCompressedSamples = getCompressedSamples();

        if (!swCompressedSamples.isOK()) {
            return swCompressedSamples.getStatus();
        }

        // Set the new sample as the current reference document as we have to start all over
        _reset(sample, date);
        return {std::tuple<ConstDataRange, FTDCCompressor::CompressorState, Date_t>(
            std::get<0>(swCompressedSamples.getValue()),
            CompressorState::kSchemaChanged, //schema发生了变化
            std::get<1>(swCompressedSamples.getValue()))};
    }


    // Add another sample   _deltas中的取值类型只会是isFTDCType，参考db.runCommand({getDiagnosticData:1})
    for (std::size_t i = 0; i < _metrics.size(); ++i) {
        // NOTE: This touches a lot of cache lines so that compression code can be more effcient.
        _deltas[getArrayOffset(_maxDeltas, _deltaCount, i)] = _metrics[i] - _prevmetrics[i];
    }

    ++_deltaCount;

    _prevmetrics.clear();
    swap(_prevmetrics, _metrics);

    // If the count is full, flush
    //诊断采样次数
    if (_deltaCount == _maxDeltas) {
        auto swCompressedSamples = getCompressedSamples();

        if (!swCompressedSamples.isOK()) {
            return swCompressedSamples.getStatus();
        }

        // Setup so that we treat the next sample as the reference sample
        _referenceDoc = BSONObj();

        return {std::tuple<ConstDataRange, FTDCCompressor::CompressorState, Date_t>(
            std::get<0>(swCompressedSamples.getValue()),
            CompressorState::kCompressorFull, //diagnosticDataCollectionSamplesPerChunk采样数满了
            std::get<1>(swCompressedSamples.getValue()))};
    }

    // The buffer is not full, inform the caller
    return {boost::none};
}

//FTDCFileWriter::writeSample
StatusWith<std::tuple<ConstDataRange, Date_t>> FTDCCompressor::getCompressedSamples() {
    _uncompressedChunkBuffer.setlen(0);

	LOGV2_DEBUG(220427, 2, "FTDCCompressor::getCompressedSamples ", "_metricsCount:"_attr = _metricsCount, "_deltaCount:"_attr = _deltaCount);
	LOGV2_DEBUG(220427, 2, "FTDCCompressor::getCompressedSamples","_referenceDoc x: "_attr = _referenceDoc);
    // Append reference document - BSON Object
    //_referenceDoc先加入到_uncompressedChunkBuffer
    _uncompressedChunkBuffer.appendBuf(_referenceDoc.objdata(), _referenceDoc.objsize());

    // Append count of metrics - uint32 little endian
    _uncompressedChunkBuffer.appendNum(static_cast<std::uint32_t>(_metricsCount));

    // Append count of samples - uint32 little endian
    _uncompressedChunkBuffer.appendNum(static_cast<std::uint32_t>(_deltaCount));

    if (_metricsCount != 0 && _deltaCount != 0) {
        // On average, we do not need all 10 bytes for every sample, worst case, we grow the buffer
        DataBuilder db(_metricsCount * _deltaCount * FTDCVarInt::kMaxSizeBytes64 / 2);

        std::uint32_t zeroesCount = 0;

        // For each set of samples for a particular metric,
        // we think of it is simple array of 64-bit integers we try to compress into a byte array.
        // This is done in three steps for each metric
        // 1. Delta Compression
        //   - i.e., we store the difference between pairs of samples, not their absolute values
        //   - this is done in addSamples
        // 2. Run Length Encoding of zeros
        //   - We find consecutive sets of zeros and represent them as a tuple of (0, count - 1).
        //   - Each memeber is stored as VarInt packed integer
        // 3. Finally, for non-zero members, we store these as VarInt packed
        //
        // These byte arrays are added to a buffer which is then concatenated with other chunks and
        // compressed with ZLIB.
        for (std::uint32_t i = 0; i < _metricsCount; i++) {
            for (std::uint32_t j = 0; j < _deltaCount; j++) {
                std::uint64_t delta = _deltas[getArrayOffset(_maxDeltas, j, i)];

                if (delta == 0) {
                    ++zeroesCount;
                    continue;
                }

				//连续的多个0先添加到_uncompressedChunkBuffer
                // If we have a non-zero sample, then write out all the accumulated zero samples.
                //例如连续10个metrics都为0，则记录0，9,第一个0代表后面的metrics为0，除了第一个0还有9个，一共10个metrics为0
                if (zeroesCount > 0) {
                    auto s1 = db.writeAndAdvance(FTDCVarInt(0)); //值为0的记录下来
                    if (!s1.isOK()) {
                        return s1; //异常直接返回
                    }

                    auto s2 = db.writeAndAdvance(FTDCVarInt(zeroesCount - 1));
                    if (!s2.isOK()) {
                        return s2;
                    }

                    zeroesCount = 0;
                }

				//非0的metrics直接记录
                auto s3 = db.writeAndAdvance(FTDCVarInt(delta));
                if (!s3.isOK()) {
                    return s3;
                }
            }

            // If we are on the last metric, and the previous loop ended in a zero, write out the
            // RLE
            // pair of zero information.
            if ((i == (_metricsCount - 1)) && zeroesCount) {
                auto s1 = db.writeAndAdvance(FTDCVarInt(0));
                if (!s1.isOK()) {
                    return s1;
                }

                auto s2 = db.writeAndAdvance(FTDCVarInt(zeroesCount - 1));
                if (!s2.isOK()) {
                    return s2;
                }
            }
        }

		//增量数据添加到_uncompressedChunkBuffer末尾
        // Append the entire compacted metric chunk into the uncompressed buffer
        ConstDataRange cdr = db.getCursor();
        _uncompressedChunkBuffer.appendBuf(cdr.data(), cdr.length());
    }

	//压缩
    auto swDest = _compressor.compress(
        ConstDataRange(_uncompressedChunkBuffer.buf(), _uncompressedChunkBuffer.len()));

    // The only way for compression to fail is if the buffer size calculations are wrong
    if (!swDest.isOK()) {
        return swDest.getStatus();
    }

    _compressedChunkBuffer.setlen(0);

    _compressedChunkBuffer.appendNum(static_cast<std::uint32_t>(_uncompressedChunkBuffer.len()));

    _compressedChunkBuffer.appendBuf(swDest.getValue().data(), swDest.getValue().length());

    return std::tuple<ConstDataRange, Date_t>(
        ConstDataRange(_compressedChunkBuffer.buf(),
                       static_cast<size_t>(_compressedChunkBuffer.len())),
        _referenceDocDate);
}

void FTDCCompressor::reset() {
    _metrics.clear();
    _reset(BSONObj(), Date_t());
}

//FTDCCompressor::addSample
void FTDCCompressor::_reset(const BSONObj& referenceDoc, Date_t date) {
    _referenceDoc = referenceDoc;
    _referenceDocDate = date;

    _metricsCount = _metrics.size();
    _deltaCount = 0;
    _prevmetrics.clear();
    swap(_prevmetrics, _metrics);

    // The reference document counts as the first sample, remaining samples
    // are delta encoded, so the maximum number of deltas is one less than
    // the configured number of samples.
    _maxDeltas = _config->maxSamplesPerArchiveMetricChunk - 1;
    _deltas.resize(_metricsCount * _maxDeltas);
}

}  // namespace mongo
