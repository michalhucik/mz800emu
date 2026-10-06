/*
 * qd_image.h - Sharp Quick Disk .qd image codec
 *
 * Supports the legacy logical Sharp/MZ stream and the physical HxC and
 * FlashFloppy containers.  The decoder returns the logical byte stream used
 * internally by the MZ-1F11 emulation (the same framing as an .mzq image).
 * The encoder performs the inverse conversion while preserving the physical
 * container profile captured during decode.
 */

#ifndef MZ_QD_IMAGE_H
#define MZ_QD_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mz_qd_image_format {
    MZ_QD_IMAGE_FORMAT_UNKNOWN = 0,
    MZ_QD_IMAGE_FORMAT_SHARP_LOGICAL,
    MZ_QD_IMAGE_FORMAT_HXC,
    MZ_QD_IMAGE_FORMAT_FLASHFLOPPY
} mz_qd_image_format_t;

typedef enum mz_qd_image_error {
    MZ_QD_IMAGE_OK = 0,
    MZ_QD_IMAGE_ERROR_ARGUMENT,
    MZ_QD_IMAGE_ERROR_FORMAT,
    MZ_QD_IMAGE_ERROR_TRUNCATED,
    MZ_QD_IMAGE_ERROR_UNSUPPORTED,
    MZ_QD_IMAGE_ERROR_CORRUPT,
    MZ_QD_IMAGE_ERROR_SEQUENCE,
    MZ_QD_IMAGE_ERROR_CAPACITY,
    MZ_QD_IMAGE_ERROR_MEMORY
} mz_qd_image_error_t;

/** Container details required to save a decoded image in the same format. */
typedef struct mz_qd_image_profile {
    mz_qd_image_format_t format;
    uint32_t container_size;
    uint32_t descriptor_offset;
    uint32_t track_offset;
    uint32_t track_length;
    uint32_t stored_track_length;
    uint32_t window_start;
    uint32_t window_end;
    uint32_t bit_rate;
    uint8_t blank_filler;
} mz_qd_image_profile_t;

/** Fill profile with the canonical geometry for a newly-created image. */
mz_qd_image_error_t mz_qd_image_profile_init_default (
                                         mz_qd_image_profile_t *profile,
                                         mz_qd_image_format_t format );

/** Detect a supported .qd representation from its contents. */
mz_qd_image_format_t mz_qd_image_detect ( const uint8_t *image, size_t image_size );

/**
 * Validate and decode a .qd image into an allocated logical MZQ byte stream.
 *
 * The returned buffer is allocated with malloc() and must be released with
 * free().  Output arguments are cleared on failure.
 */
mz_qd_image_error_t mz_qd_image_decode ( const uint8_t *image,
                                         size_t image_size,
                                         uint8_t **logical_image,
                                         size_t *logical_size,
                                         mz_qd_image_format_t *format );

/** Decode an image and retain the source container layout for later saving. */
mz_qd_image_error_t mz_qd_image_decode_with_profile (
                                         const uint8_t *image,
                                         size_t image_size,
                                         uint8_t **logical_image,
                                         size_t *logical_size,
                                         mz_qd_image_profile_t *profile );

/**
 * Encode a logical MZQ stream into the representation described by profile.
 *
 * The returned buffer is allocated with malloc() and must be released with
 * free(). Output arguments are cleared on failure.
 */
mz_qd_image_error_t mz_qd_image_encode ( const uint8_t *logical_image,
                                         size_t logical_size,
                                         const mz_qd_image_profile_t *profile,
                                         uint8_t **image,
                                         size_t *image_size );

/** Stable English diagnostic for a codec result. */
const char *mz_qd_image_error_string ( mz_qd_image_error_t error );

#ifdef __cplusplus
}
#endif

#endif /* MZ_QD_IMAGE_H */
