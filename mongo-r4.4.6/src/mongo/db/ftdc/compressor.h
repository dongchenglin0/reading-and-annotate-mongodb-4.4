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

#pragma once

#include <boost/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/ftdc/block_compressor.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * FTDCCompressor is responsible for taking a set of BSON documents containing metrics, and
 * compressing them into a highly compressed buffer. Metrics are defined as BSON number or number
 * like type (like dates, and timestamps).
 *
 * Compression Method
 * 1. For each document after the first, it computes the delta between it and the preceding document
 *    for the number fields
 * 2. It stores the deltas into an array of std::int64_t.
 * 3. It compressed each std::int64_t using VarInt integer compression. See varint.h.
 * 4. Encodes zeros in Run Length Encoded pairs of <Count, Zero>
 * 5. ZLIB compresses the final processed array
 *
 * NOTE: This compression ignores non-number data, and assumes the non-number data is constant
 * across all documents in the series of documents.
 */
class FTDCCompressor {
    FTDCCompressor(const FTDCCompressor&) = delete;
    FTDCCompressor& operator=(const FTDCCompressor&) = delete;

public:
    /**
     * The FTDCCompressor returns one of these values when a sample is added to indicate whether the
     * caller should flush the buffer to disk or not.
     */
    enum class CompressorState {
        /**
         * Needs to flush because the schemas has changed. Caller needs to flush.
         */
        kSchemaChanged,

        /**
         * Quota on the number of samples in a metric chunk has been reached. Caller needs to flush.
         */
        kCompressorFull,
    };

    explicit FTDCCompressor(const FTDCConfig* config) : _config(config) {}

    /**
     * Add a bson document containing metrics into the compressor.
     *
     * Returns flag indicating whether the caller should flush the compressor buffer to disk.
     *  1. kCompressorFull if the compressor is considered full.
     *  2. kSchemaChanged if there was a schema change, and buffer should be flushed.
     *  3. kHasSpace if it has room for more metrics in the current buffer.
     *
     * date is the date at which the sample as started to be captured. It will be saved in the
     * compressor if this sample is used as the reference document.N
     */
    StatusWith<boost::optional<std::tuple<ConstDataRange, CompressorState, Date_t>>> addSample(
        const BSONObj& sample, Date_t date);

    /**
     * Returns the number of enqueued samples.
     *
     * The a buffer will decompress to (1 + getCountCount). The extra 1 comes
     * from the reference document.
     */

    //采样数
    std::size_t getSampleCount() const {
        // TODO: This method should probably be renamed, since it currently
        // returns the number of deltas, which does not include the sample
        // implicitly contained in the reference document.
        return _deltaCount;
    }

    /**
     * Has a document been added?
     *
     * If addSample has been called, then we have at least the reference document, but not
     * necessarily any additional metric samples. When the buffer is filled to capacity,
     * the reference document is reset.
     */
    bool hasDataToFlush() const {
        return !_referenceDoc.isEmpty();
    }

    /**
     * Gets buffer of compressed data contained in the FTDCCompressor.
     *
     * The returned buffer is valid until next call to addSample() or getCompressedSamples() with
     * CompressBuffer::kGenerateNewCompressedBuffer.
     */
    StatusWith<std::tuple<ConstDataRange, Date_t>> getCompressedSamples();

    /**
     * Reset the state of the compressor.
     *
     * Callers can use this to reset the compressor to a clean state instead of recreating it.
     */
    void reset();

    /**
     * Compute the offset into an array for given (sample, metric) pair
     */
    static size_t getArrayOffset(std::uint32_t sampleCount,
                                 std::uint32_t sample,
                                 std::uint32_t metric) {
        return metric * sampleCount + sample;
    }

private:
    /**
     * Reset the state
     */
    void _reset(const BSONObj& referenceDoc, Date_t date);

private:
    // Block Compressor
    BlockCompressor _compressor;

    // Config
    const FTDCConfig* const _config;

    // Reference schema document
    BSONObj _referenceDoc;

    // Time at which reference schema document was collected.
    // Passed in via addSample and returned with each chunk.
    Date_t _referenceDocDate;

    // Number of Metrics for the reference document
    //多少个Metric，一个Metrics对应一次诊断项中的所有的Metric
    std::uint32_t _metricsCount{0};

    // Number of deltas recorded
    //实际上就是采样次数，见getSampleCount
    std::uint32_t _deltaCount{0};

    // Max deltas for the current chunk
    //maxSamplesPerArchiveMetricChunk配置-1
    std::size_t _maxDeltas{0};

    // Array of deltas - M x S
    // _deltas[Metrics][Metrics]
    //数组中连续的多次诊断项Metrics信息连续存在数组空间，参考FTDCCompressor::addSample->FTDCCompressor::_reset
    std::vector<std::uint64_t> _deltas;

    // Buffer for metric chunk compressed = uncompressed length + compressed data
    BufBuilder _compressedChunkBuffer;

    // Buffer for uncompressed metric chunk
    BufBuilder _uncompressedChunkBuffer;

    // Buffer to hold metrics
    //本次采集的诊断项目信息存到这里，参考extractMetricsFromDocument
    std::vector<std::uint64_t> _metrics;
    std::vector<std::uint64_t> _prevmetrics;
};

}  // namespace mongo
