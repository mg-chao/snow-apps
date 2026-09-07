#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnowStitchFramePoolImpl SnowStitchFramePool;
typedef struct SnowStitchFrameBufferImpl SnowStitchFrameBuffer;
typedef struct SnowStitchSessionImpl SnowStitchSession;
typedef struct SnowStitchSnapshotImpl SnowStitchSnapshot;
typedef struct SnowStitchOwnedImageImpl SnowStitchOwnedImage;
typedef struct SnowStitchExportTaskImpl SnowStitchExportTask;

#if defined(SNOW_STITCH_PERF_INSTRUMENTATION)
typedef enum SnowStitchPerfStage {
    SNOW_STITCH_PERF_FRAME_FREEZE = 0,
    SNOW_STITCH_PERF_DUPLICATE_CHECK = 1,
    SNOW_STITCH_PERF_REFERENCE_PREPARATION = 2,
    SNOW_STITCH_PERF_GRAYSCALE = 3,
    SNOW_STITCH_PERF_SIMILARITY_MAPS = 4,
    SNOW_STITCH_PERF_FEATURE_EXTRACTION = 5,
    SNOW_STITCH_PERF_DESCRIPTOR_MATCHING = 6,
    SNOW_STITCH_PERF_CANDIDATE_SCORING = 7,
    SNOW_STITCH_PERF_REFINEMENT = 8,
    SNOW_STITCH_PERF_REGION_UPDATE = 9,
    SNOW_STITCH_PERF_CANVAS_COMPOSITION = 10,
    SNOW_STITCH_PERF_REFERENCE_SYNTHESIS = 11,
    SNOW_STITCH_PERF_PREVIEW_SCALING = 12,
    SNOW_STITCH_PERF_PUSH_TOTAL = 13,
    SNOW_STITCH_PERF_INITIALIZATION = 14,
    SNOW_STITCH_PERF_RESERVED = 15,
    SNOW_STITCH_PERF_STAGE_COUNT = 16,
} SnowStitchPerfStage;
typedef struct SnowStitchPerfSnapshot {
    uint64_t elapsed_ns[SNOW_STITCH_PERF_STAGE_COUNT];
    uint64_t calls[SNOW_STITCH_PERF_STAGE_COUNT];
} SnowStitchPerfSnapshot;
/* Reset/read the calling thread only. Read preserves the counters. */
void snow_stitch_perf_reset_thread(void);
uint8_t snow_stitch_perf_read_thread(SnowStitchPerfSnapshot* output, size_t output_size);
#endif

typedef enum SnowStitchFrameEvent {
    SNOW_STITCH_FRAME_EVENT_INITIAL = 0,
    SNOW_STITCH_FRAME_EVENT_EXTENDED_TOP = 1,
    SNOW_STITCH_FRAME_EVENT_EXTENDED_BOTTOM = 2,
    SNOW_STITCH_FRAME_EVENT_COVERED = 3,
    SNOW_STITCH_FRAME_EVENT_DUPLICATE = 4,
    SNOW_STITCH_FRAME_EVENT_UNMATCHED = 5,
    SNOW_STITCH_FRAME_EVENT_EXTENDED_LEFT = 6,
    SNOW_STITCH_FRAME_EVENT_EXTENDED_RIGHT = 7,
} SnowStitchFrameEvent;

typedef enum SnowStitchAxis {
    SNOW_STITCH_AXIS_VERTICAL = 0,
    SNOW_STITCH_AXIS_HORIZONTAL = 1,
} SnowStitchAxis;

typedef enum SnowStitchUnmatchedReason {
    SNOW_STITCH_UNMATCHED_REASON_NONE = 0,
    SNOW_STITCH_UNMATCHED_REASON_INSUFFICIENT_OVERLAP = 1,
    SNOW_STITCH_UNMATCHED_REASON_LOW_INFORMATION = 2,
    SNOW_STITCH_UNMATCHED_REASON_AMBIGUOUS = 3,
    SNOW_STITCH_UNMATCHED_REASON_CONFLICTING_REFERENCES = 4,
    SNOW_STITCH_UNMATCHED_REASON_FIXED_CONTENT_DOMINATED = 5,
    SNOW_STITCH_UNMATCHED_REASON_VERIFICATION_FAILED = 6,
} SnowStitchUnmatchedReason;

typedef struct SnowStitchConfig {
    uint32_t struct_size;
    uint32_t max_output_height;
    uint64_t max_output_pixels;
    uint32_t min_overlap_rows;
    float min_overlap_ratio;
    uint32_t accepted_history_capacity;
    /* Consumes one formerly reserved slot; the structure size is unchanged. */
    uint32_t axis;
    uint32_t reserved[8];
} SnowStitchConfig;

typedef struct SnowStitchMatchMetrics {
    float score;
    float second_score;
    float content_coverage;
    float fixed_coverage;
    float inlier_ratio;
    uint32_t feature_support;
    uint32_t reference_count;
} SnowStitchMatchMetrics;

typedef struct SnowStitchFrameOutcome {
    SnowStitchFrameEvent event;
    SnowStitchUnmatchedReason unmatched_reason;
    int32_t matched_reference_offset_y;
    uint8_t has_matched_reference_offset_y;
    uint8_t reserved[3];
    SnowStitchMatchMetrics metrics;
    uint32_t added_rows;
    uint32_t output_width;
    uint32_t output_height;
    /* Full edge band rewritten by this frame, including splice overlap. */
    uint32_t delta_top;
    uint32_t delta_rows;
} SnowStitchFrameOutcome;

typedef struct SnowStitchMutableImageInfo {
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowStitchMutableImageInfo;

typedef struct SnowStitchImageInfo {
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    const uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowStitchImageInfo;

typedef enum SnowStitchPngCompression {
    SNOW_STITCH_PNG_COMPRESSION_FAST = 0,
    SNOW_STITCH_PNG_COMPRESSION_BALANCED = 1,
    SNOW_STITCH_PNG_COMPRESSION_BEST = 2,
} SnowStitchPngCompression;

typedef enum SnowStitchExportStage {
    SNOW_STITCH_EXPORT_STAGE_PREPARING = 0,
    SNOW_STITCH_EXPORT_STAGE_ENCODING = 1,
    SNOW_STITCH_EXPORT_STAGE_COMMITTING = 2,
} SnowStitchExportStage;

typedef enum SnowStitchExportStatus {
    SNOW_STITCH_EXPORT_STATUS_RUNNING = 0,
    SNOW_STITCH_EXPORT_STATUS_COMPLETE = 1,
    SNOW_STITCH_EXPORT_STATUS_FAILED = 2,
    SNOW_STITCH_EXPORT_STATUS_CANCELED = 3,
} SnowStitchExportStatus;

typedef struct SnowStitchPngExportConfig {
    const char* output_path_utf8;
    SnowStitchPngCompression compression;
    uint8_t overwrite;
    uint8_t reserved[31];
} SnowStitchPngExportConfig;

typedef struct SnowStitchExportProgress {
    SnowStitchExportStage stage;
    uint32_t rows_written;
    uint32_t total_rows;
    float percent;
} SnowStitchExportProgress;

SnowStitchFramePool* snow_stitch_frame_pool_create(uint32_t width, uint32_t height,
                                                   size_t capacity);
void snow_stitch_frame_pool_destroy(SnowStitchFramePool* pool);
/* Returns NULL while all bounded slots are owned by the pipeline. */
SnowStitchFrameBuffer* snow_stitch_frame_pool_acquire(SnowStitchFramePool* pool);
/* The writable pixel pointer remains valid until the frame is consumed/destroyed. */
uint8_t snow_stitch_frame_buffer_info(SnowStitchFrameBuffer* frame,
                                      SnowStitchMutableImageInfo* out_info);
void snow_stitch_frame_buffer_destroy(SnowStitchFrameBuffer* frame);

uint8_t snow_stitch_config_default(SnowStitchConfig* out_config);
SnowStitchSession* snow_stitch_session_create(const SnowStitchConfig* config);
void snow_stitch_session_destroy(SnowStitchSession* session);
uint8_t snow_stitch_session_reset(SnowStitchSession* session);
/* Always consumes *inout_frame and sets the caller's slot to NULL. */
uint8_t snow_stitch_session_push_owned(SnowStitchSession* session,
                                       SnowStitchFrameBuffer** inout_frame,
                                       SnowStitchFrameOutcome* out_outcome);
uint8_t snow_stitch_session_copy_rows(const SnowStitchSession* session, uint32_t top, uint32_t rows,
                                      uint8_t* destination, size_t destination_len);
SnowStitchOwnedImage* snow_stitch_session_materialize_rows(const SnowStitchSession* session,
                                                           uint32_t top, uint32_t bottom);
SnowStitchOwnedImage* snow_stitch_session_materialize_axis(const SnowStitchSession* session,
                                                           uint32_t start, uint32_t end);
SnowStitchOwnedImage* snow_stitch_session_render_scaled_rows(const SnowStitchSession* session,
                                                             uint32_t top, uint32_t rows,
                                                             uint32_t width, uint32_t height);
SnowStitchOwnedImage* snow_stitch_session_render_scaled_axis(const SnowStitchSession* session,
                                                             uint32_t start, uint32_t span,
                                                             uint32_t width, uint32_t height);
SnowStitchSnapshot* snow_stitch_session_snapshot(const SnowStitchSession* session,
                                                 uint32_t trim_top, uint32_t trim_bottom);
SnowStitchSnapshot* snow_stitch_session_snapshot_axis(const SnowStitchSession* session,
                                                      uint32_t start, uint32_t end);

/* Snapshots retain shared immutable canvas tiles and do not depend on session lifetime.
 * Creating
 * or slicing a snapshot does not copy canvas pixels; materialization does. */
void snow_stitch_snapshot_destroy(SnowStitchSnapshot* snapshot);
uint8_t snow_stitch_snapshot_info(const SnowStitchSnapshot* snapshot,
                                  SnowStitchImageInfo* out_info);
/* Copies tightly packed RGBA rows into a caller-owned strided destination. */
uint8_t snow_stitch_snapshot_copy_rows(const SnowStitchSnapshot* snapshot, uint32_t top,
                                       uint32_t rows, size_t destination_stride,
                                       uint8_t* destination, size_t destination_len);
SnowStitchSnapshot* snow_stitch_snapshot_slice_rows(const SnowStitchSnapshot* snapshot,
                                                     uint32_t top, uint32_t bottom);
SnowStitchSnapshot* snow_stitch_snapshot_slice_axis(const SnowStitchSnapshot* snapshot,
                                                    uint32_t start, uint32_t end);
SnowStitchOwnedImage* snow_stitch_snapshot_materialize(const SnowStitchSnapshot* snapshot);
SnowStitchOwnedImage* snow_stitch_snapshot_render_scaled(const SnowStitchSnapshot* snapshot,
                                                         uint32_t width, uint32_t height);

uint8_t snow_stitch_owned_image_info(const SnowStitchOwnedImage* image,
                                     SnowStitchImageInfo* out_info);
/* Invalidates the pixel pointer returned by snow_stitch_owned_image_info. */
void snow_stitch_owned_image_destroy(SnowStitchOwnedImage* image);

SnowStitchExportTask* snow_stitch_snapshot_export_png(const SnowStitchSnapshot* snapshot,
                                                      const SnowStitchPngExportConfig* config);
void snow_stitch_export_task_cancel(SnowStitchExportTask* task);
SnowStitchExportStatus snow_stitch_export_task_poll(SnowStitchExportTask* task,
                                                    SnowStitchExportProgress* out_progress,
                                                    uint8_t* out_has_progress);
SnowStitchExportStatus snow_stitch_export_task_wait(SnowStitchExportTask* task);
const char* snow_stitch_export_task_error_message(const SnowStitchExportTask* task);
/* Cancels and joins a still-running background export before returning. */
void snow_stitch_export_task_destroy(SnowStitchExportTask* task);

const char* snow_stitch_last_error_message(void);

#ifdef __cplusplus
}
#endif
