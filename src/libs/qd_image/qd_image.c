/*
 * qd_image.c - Sharp Quick Disk .qd codec
 *
 * Physical HxC/FlashFloppy images store an LSB-first MFM bit-cell stream.
 * There is no guaranteed byte phase, so all sixteen possible data-cell
 * phases are decoded.  Only Sharp frames preceded by break/sync bytes and
 * passing the on-disk CRC are accepted.
 */

#include "qd_image.h"

#include <stdlib.h>
#include <string.h>

#define QD_MAX_FILES              50u
#define QD_MAX_BLOCKS             ( QD_MAX_FILES * 2u )
#define QD_HXC_HEADER_SIZE        40u
#define QD_DESCRIPTOR_OFFSET_FF   0x200u
#define QD_DESCRIPTOR_SIZE        16u
#define QD_MIN_TRACK_OFFSET       0x400u
#define QD_HEADER_DATA_SIZE       64u
#define QD_PHYSICAL_HEADER_SIZE   70u
#define QD_LEGACY_IMAGE_SIZE      0xf00fu
#define QD_LEGACY_MIN_TAIL_SIZE   8u
#define QD_QDF_IMAGE_SIZE          81936u
#define QD_QDF_SIGNATURE_SIZE      16u
#define QD_QDF_HEADER_OFFSET       0x12eau
#define QD_QDF_HEADER_SIZE         7655u
#define QD_QDF_FILE_OVERHEAD       620u
#define QD_HXC_MARIO_FNBLK_READY_OFFSET_BITS 22u
#define QD_HXC_MFM_SHIFT_BITS       5u
#define QD_HXC_COUNT_PRE_SYNC       9u
#define QD_HXC_COUNT_POST_SYNC      6u
#define QD_HXC_COUNT_HEADER_GAP     2736u
#define QD_HXC_BLOCK_PRE_SYNC       10u
#define QD_HXC_BLOCK_POST_SYNC      7u
#define QD_HXC_HEADER_BODY_GAP      254u
#define QD_HXC_BODY_HEADER_GAP      256u

static const uint8_t s_logical_start[4] = { 0x00, 0x16, 0x16, 0xa5 };
static const uint8_t s_logical_crc[3] = { 'C', 'R', 'C' };
static const uint8_t s_hxc_signature[8] = { 'H', 'X', 'C', 'Q', 'D', 'D', 'R', 'V' };
static const uint8_t s_qdf_signature[QD_QDF_SIGNATURE_SIZE] = {
    '-', 'Q', 'D', ' ', 'f', 'o', 'r', 'm', 'a', 't', '-',
    0xff, 0xff, 0xff, 0xff, 0xff
};

mz_qd_image_error_t mz_qd_image_profile_init_default (
                                         mz_qd_image_profile_t *profile,
                                         mz_qd_image_format_t format ) {
    if ( profile == NULL ) return MZ_QD_IMAGE_ERROR_ARGUMENT;
    memset ( profile, 0, sizeof ( *profile ) );
    profile->format = format;

    if ( format == MZ_QD_IMAGE_FORMAT_SHARP_LOGICAL ) {
        profile->container_size = QD_LEGACY_IMAGE_SIZE;
        return MZ_QD_IMAGE_OK;
    };

    profile->container_size = 0x32000u;
    profile->descriptor_offset = 0x200u;
    profile->track_offset = 0x400u;
    profile->stored_track_length = 0x31c00u;
    if ( format == MZ_QD_IMAGE_FORMAT_HXC ) {
        profile->track_length = 0x31c00u;
        profile->window_start = 0x3200u;
        profile->window_end = 0x25600u;
        profile->bit_rate = 203388u;
        profile->blank_filler = 0x01u;
        return MZ_QD_IMAGE_OK;
    };
    if ( format == MZ_QD_IMAGE_FORMAT_FLASHFLOPPY ) {
        profile->track_length = 0x31a99u;
        profile->window_start = 0x31a9u;
        profile->window_end = 0x2b745u;
        profile->blank_filler = 0x11u;
        return MZ_QD_IMAGE_OK;
    };

    memset ( profile, 0, sizeof ( *profile ) );
    return MZ_QD_IMAGE_ERROR_ARGUMENT;
}

typedef struct qd_frame {
    size_t position;
    uint8_t type;
    size_t size;
    uint8_t *bytes;
} qd_frame_t;

typedef struct qd_frame_list {
    qd_frame_t *items;
    size_t count;
    size_t capacity;
} qd_frame_list_t;

static uint16_t read_le16 ( const uint8_t *p ) {
    return (uint16_t) ( (uint16_t) p[0] | ( (uint16_t) p[1] << 8 ) );
}

static uint32_t read_le32 ( const uint8_t *p ) {
    return (uint32_t) p[0]
         | ( (uint32_t) p[1] << 8 )
         | ( (uint32_t) p[2] << 16 )
         | ( (uint32_t) p[3] << 24 );
}

static void write_le32 ( uint8_t *p, uint32_t value ) {
    p[0] = (uint8_t) value;
    p[1] = (uint8_t) ( value >> 8 );
    p[2] = (uint8_t) ( value >> 16 );
    p[3] = (uint8_t) ( value >> 24 );
}

static int has_logical_start ( const uint8_t *p ) {
    return memcmp ( p, s_logical_start, sizeof ( s_logical_start ) ) == 0;
}

mz_qd_image_format_t mz_qd_image_detect ( const uint8_t *image, size_t image_size ) {
    if ( image == NULL ) return MZ_QD_IMAGE_FORMAT_UNKNOWN;

    if ( image_size >= sizeof ( s_hxc_signature )
      && memcmp ( image, s_hxc_signature, sizeof ( s_hxc_signature ) ) == 0 ) {
        return MZ_QD_IMAGE_FORMAT_HXC;
    }

    if ( image_size >= 5 && image[3] == 'Q' && image[4] == 'D' ) {
        return MZ_QD_IMAGE_FORMAT_FLASHFLOPPY;
    }

    if ( image_size >= 8
      && has_logical_start ( image )
      && memcmp ( image + 5, s_logical_crc, sizeof ( s_logical_crc ) ) == 0 ) {
        return MZ_QD_IMAGE_FORMAT_SHARP_LOGICAL;
    }

    return MZ_QD_IMAGE_FORMAT_UNKNOWN;
}

static mz_qd_image_error_t validate_logical ( const uint8_t *image,
                                               size_t image_size,
                                               size_t *content_size ) {
    size_t position = 8;
    unsigned block_count;
    unsigned file;

    if ( image_size < 8 ) return MZ_QD_IMAGE_ERROR_TRUNCATED;
    block_count = image[4];
    if ( ( block_count & 1u ) != 0 || block_count > QD_MAX_BLOCKS ) {
        return MZ_QD_IMAGE_ERROR_SEQUENCE;
    }

    for ( file = 0; file < block_count / 2u; ++file ) {
        uint16_t header_body_size;
        uint16_t body_size;

        if ( position > image_size || image_size - position < 74u ) {
            return MZ_QD_IMAGE_ERROR_TRUNCATED;
        }
        if ( !has_logical_start ( image + position )
          || image[position + 4] != 0x00
          || read_le16 ( image + position + 5 ) != QD_HEADER_DATA_SIZE
          || memcmp ( image + position + 71, s_logical_crc, 3 ) != 0 ) {
            return MZ_QD_IMAGE_ERROR_CORRUPT;
        }
        header_body_size = read_le16 ( image + position + 27 );
        position += 74u;

        if ( position > image_size || image_size - position < 10u ) {
            return MZ_QD_IMAGE_ERROR_TRUNCATED;
        }
        if ( !has_logical_start ( image + position ) || image[position + 4] != 0x05 ) {
            return MZ_QD_IMAGE_ERROR_CORRUPT;
        }
        body_size = read_le16 ( image + position + 5 );
        if ( body_size != header_body_size ) return MZ_QD_IMAGE_ERROR_SEQUENCE;
        if ( image_size - position < (size_t) body_size + 10u ) {
            return MZ_QD_IMAGE_ERROR_TRUNCATED;
        }
        if ( memcmp ( image + position + 7u + body_size, s_logical_crc, 3 ) != 0 ) {
            return MZ_QD_IMAGE_ERROR_CORRUPT;
        }
        position += (size_t) body_size + 10u;
    }

    if ( content_size != NULL ) *content_size = position;
    return MZ_QD_IMAGE_OK;
}

static uint16_t qd_crc_update ( uint16_t crc, uint8_t data ) {
    unsigned bit;
    for ( bit = 0; bit < 8; ++bit ) {
        unsigned x = data & 1u;
        data >>= 1;
        if ( crc & 0x8000u ) x ^= 1u;
        crc = (uint16_t) ( crc << 1 );
        if ( x ) crc ^= 0x8005u;
    }
    return crc;
}

static int has_valid_crc ( const uint8_t *frame, size_t size ) {
    uint16_t crc = 0;
    size_t i;
    for ( i = 0; i < size; ++i ) crc = qd_crc_update ( crc, frame[i] );
    return crc == 0;
}

static uint8_t reverse_bits ( uint8_t value ) {
    value = (uint8_t) ( ( ( value & 0x55u ) << 1 ) | ( ( value >> 1 ) & 0x55u ) );
    value = (uint8_t) ( ( ( value & 0x33u ) << 2 ) | ( ( value >> 2 ) & 0x33u ) );
    return (uint8_t) ( ( value << 4 ) | ( value >> 4 ) );
}

static void append_crc ( uint8_t *frame, size_t size_without_crc ) {
    uint16_t crc = 0;
    size_t i;
    for ( i = 0; i < size_without_crc; ++i ) {
        crc = qd_crc_update ( crc, frame[i] );
    }
    frame[size_without_crc] = reverse_bits ( (uint8_t) ( crc >> 8 ) );
    frame[size_without_crc + 1u] = reverse_bits ( (uint8_t) crc );
}

static int get_lsb_first_bit ( const uint8_t *bytes, size_t position ) {
    return ( bytes[position >> 3] & ( 1u << ( position & 7u ) ) ) != 0;
}

static uint8_t *decode_mfm_phase ( const uint8_t *track,
                                   size_t track_size,
                                   unsigned phase,
                                   size_t *decoded_size ) {
    size_t bit_count;
    size_t byte_count;
    size_t index;
    uint8_t *decoded;

    *decoded_size = 0;
    if ( track_size > ( (size_t) -1 ) / 8u ) return NULL;
    bit_count = track_size * 8u;
    if ( bit_count <= (size_t) phase + 14u ) return NULL;
    byte_count = ( bit_count - phase + 1u ) / 16u;
    decoded = (uint8_t*) malloc ( byte_count );
    if ( decoded == NULL ) return NULL;

    for ( index = 0; index < byte_count; ++index ) {
        size_t first_cell = (size_t) phase + index * 16u;
        uint8_t value = 0;
        unsigned bit;
        for ( bit = 0; bit < 8; ++bit ) {
            if ( get_lsb_first_bit ( track, first_cell + bit * 2u ) ) {
                value |= (uint8_t) ( 1u << bit );
            }
        }
        decoded[index] = value;
    }

    *decoded_size = byte_count;
    return decoded;
}

static int has_sharp_sync ( const uint8_t *decoded, size_t frame_index ) {
    size_t index = frame_index;
    size_t sync_count = 0;
    size_t lower_bound;

    while ( index > 0 && decoded[index - 1] == 0x16 ) {
        --index;
        ++sync_count;
    }
    if ( sync_count < 2 ) return 0;

    lower_bound = index > 17u ? index - 17u : 0u;
    while ( index > lower_bound ) {
        --index;
        if ( decoded[index] == 0x00 ) return 1;
    }
    return 0;
}

static void frame_list_free ( qd_frame_list_t *list ) {
    size_t i;
    for ( i = 0; i < list->count; ++i ) free ( list->items[i].bytes );
    free ( list->items );
    memset ( list, 0, sizeof ( *list ) );
}

static mz_qd_image_error_t frame_list_add ( qd_frame_list_t *list,
                                            size_t position,
                                            uint8_t type,
                                            const uint8_t *bytes,
                                            size_t size ) {
    size_t i;
    uint8_t *copy;

    for ( i = 0; i < list->count; ++i ) {
        const qd_frame_t *existing = &list->items[i];
        size_t distance = existing->position > position
                        ? existing->position - position : position - existing->position;
        if ( distance <= 15u && existing->size == size
          && memcmp ( existing->bytes, bytes, size ) == 0 ) {
            return MZ_QD_IMAGE_OK;
        }
    }

    if ( list->count == list->capacity ) {
        size_t new_capacity = list->capacity == 0 ? 8u : list->capacity * 2u;
        qd_frame_t *items;
        if ( new_capacity < list->capacity
          || new_capacity > ( (size_t) -1 ) / sizeof ( *items ) ) {
            return MZ_QD_IMAGE_ERROR_MEMORY;
        }
        items = (qd_frame_t*) realloc ( list->items, new_capacity * sizeof ( *items ) );
        if ( items == NULL ) return MZ_QD_IMAGE_ERROR_MEMORY;
        list->items = items;
        list->capacity = new_capacity;
    }

    copy = (uint8_t*) malloc ( size );
    if ( copy == NULL ) return MZ_QD_IMAGE_ERROR_MEMORY;
    memcpy ( copy, bytes, size );
    list->items[list->count].position = position;
    list->items[list->count].type = type;
    list->items[list->count].size = size;
    list->items[list->count].bytes = copy;
    ++list->count;
    return MZ_QD_IMAGE_OK;
}

static int compare_frames ( const void *left, const void *right ) {
    const qd_frame_t *a = (const qd_frame_t*) left;
    const qd_frame_t *b = (const qd_frame_t*) right;
    if ( a->position < b->position ) return -1;
    if ( a->position > b->position ) return 1;
    return 0;
}

static mz_qd_image_error_t find_physical_frames ( const uint8_t *track,
                                                  size_t track_size,
                                                  qd_frame_list_t *frames ) {
    unsigned phase;
    for ( phase = 0; phase < 16; ++phase ) {
        size_t decoded_size;
        uint8_t *decoded = decode_mfm_phase ( track, track_size, phase, &decoded_size );
        size_t index;
        if ( decoded == NULL ) {
            if ( decoded_size == 0
              && track_size <= ( (size_t) phase + 14u ) / 8u ) continue;
            return MZ_QD_IMAGE_ERROR_MEMORY;
        }

        for ( index = 0; index + 4u <= decoded_size; ++index ) {
            uint8_t marker;
            size_t frame_size;
            mz_qd_image_error_t error;

            if ( decoded[index] != 0xa5 || !has_sharp_sync ( decoded, index ) ) continue;
            marker = decoded[index + 1];

            if ( ( marker & 1u ) == 0
              && has_valid_crc ( decoded + index, 4u ) ) {
                error = frame_list_add ( frames, (size_t) phase + index * 16u,
                                         0x02, decoded + index, 4u );
                if ( error != MZ_QD_IMAGE_OK ) {
                    free ( decoded );
                    return error;
                }
                continue;
            }

            if ( marker != 0x00 && marker != 0x05 ) continue;
            {
                uint16_t data_size = read_le16 ( decoded + index + 2 );
                if ( marker == 0x00 && data_size != QD_HEADER_DATA_SIZE ) continue;
                frame_size = (size_t) data_size + 6u;
            }
            if ( frame_size > decoded_size - index ) continue;
            if ( !has_valid_crc ( decoded + index, frame_size ) ) continue;

            error = frame_list_add ( frames, (size_t) phase + index * 16u,
                                     marker, decoded + index, frame_size );
            if ( error != MZ_QD_IMAGE_OK ) {
                free ( decoded );
                return error;
            }
        }
        free ( decoded );
    }

    if ( frames->count > 1 ) {
        qsort ( frames->items, frames->count, sizeof ( frames->items[0] ), compare_frames );
    }
    return MZ_QD_IMAGE_OK;
}

static int is_blank_track ( const uint8_t *track, size_t track_size ) {
    size_t histogram[256] = { 0 };
    size_t dominant = 0;
    size_t required;
    size_t i;
    if ( track_size == 0 ) return 0;
    for ( i = 0; i < track_size; ++i ) ++histogram[track[i]];
    for ( i = 0; i < 256; ++i ) {
        if ( histogram[i] > dominant ) dominant = histogram[i];
    }
    required = ( track_size / 100u ) * 98u
             + ( ( track_size % 100u ) * 98u ) / 100u;
    return dominant >= required;
}

static mz_qd_image_error_t make_empty_logical ( uint8_t **output, size_t *output_size ) {
    uint8_t *logical = (uint8_t*) malloc ( 8u );
    if ( logical == NULL ) return MZ_QD_IMAGE_ERROR_MEMORY;
    memcpy ( logical, s_logical_start, 4u );
    logical[4] = 0;
    memcpy ( logical + 5, s_logical_crc, 3u );
    *output = logical;
    *output_size = 8u;
    return MZ_QD_IMAGE_OK;
}

static mz_qd_image_error_t build_logical_from_frames ( const qd_frame_list_t *frames,
                                                       uint8_t **output,
                                                       size_t *output_size ) {
    size_t count_index;
    mz_qd_image_error_t last_error = MZ_QD_IMAGE_ERROR_SEQUENCE;

    for ( count_index = 0; count_index < frames->count; ++count_index ) {
        const qd_frame_t *count_frame = &frames->items[count_index];
        const qd_frame_t *blocks[QD_MAX_BLOCKS];
        unsigned block_count;
        unsigned found = 0;
        size_t i;
        size_t logical_size = 8u;
        uint8_t *logical;
        size_t position;

        if ( count_frame->type != 0x02 ) continue;
        block_count = count_frame->bytes[1];
        if ( ( block_count & 1u ) != 0 || block_count > QD_MAX_BLOCKS ) {
            last_error = MZ_QD_IMAGE_ERROR_SEQUENCE;
            continue;
        }
        if ( block_count == 0 ) return make_empty_logical ( output, output_size );

        for ( i = count_index + 1; i < frames->count && found < block_count; ++i ) {
            if ( frames->items[i].position <= count_frame->position ) continue;
            if ( frames->items[i].type == 0x00 || frames->items[i].type == 0x05 ) {
                blocks[found++] = &frames->items[i];
            }
        }
        if ( found < block_count ) {
            last_error = MZ_QD_IMAGE_ERROR_SEQUENCE;
            continue;
        }

        for ( i = 0; i < block_count; i += 2u ) {
            uint16_t header_size;
            uint16_t body_size;
            if ( blocks[i]->type != 0x00 || blocks[i + 1u]->type != 0x05
              || blocks[i]->size != QD_PHYSICAL_HEADER_SIZE
              || blocks[i + 1u]->size < 6u ) {
                last_error = MZ_QD_IMAGE_ERROR_SEQUENCE;
                break;
            }
            header_size = read_le16 ( blocks[i]->bytes + 24u );
            body_size = read_le16 ( blocks[i + 1u]->bytes + 2u );
            if ( header_size != body_size
              || blocks[i + 1u]->size != (size_t) body_size + 6u ) {
                last_error = MZ_QD_IMAGE_ERROR_SEQUENCE;
                break;
            }
            if ( logical_size > ( (size_t) -1 ) - (size_t) body_size - 84u ) {
                return MZ_QD_IMAGE_ERROR_MEMORY;
            }
            logical_size += (size_t) body_size + 84u;
        }
        if ( i != block_count ) continue;

        logical = (uint8_t*) malloc ( logical_size );
        if ( logical == NULL ) return MZ_QD_IMAGE_ERROR_MEMORY;
        memcpy ( logical, s_logical_start, 4u );
        logical[4] = (uint8_t) block_count;
        memcpy ( logical + 5u, s_logical_crc, 3u );
        position = 8u;

        for ( i = 0; i < block_count; ++i ) {
            const qd_frame_t *frame = blocks[i];
            logical[position++] = 0x00;
            logical[position++] = 0x16;
            logical[position++] = 0x16;
            memcpy ( logical + position, frame->bytes, frame->size - 2u );
            position += frame->size - 2u;
            memcpy ( logical + position, s_logical_crc, 3u );
            position += 3u;
        }

        *output = logical;
        *output_size = logical_size;
        return MZ_QD_IMAGE_OK;
    }

    return last_error;
}

static mz_qd_image_error_t decode_physical ( const uint8_t *image,
                                             size_t image_size,
                                             mz_qd_image_format_t format,
                                             uint8_t **output,
                                             size_t *output_size,
                                             mz_qd_image_profile_t *profile ) {
    uint32_t descriptor_offset;
    uint32_t track_offset;
    uint32_t track_length;
    uint32_t window_start;
    uint32_t window_end;
    const uint8_t *descriptor;
    const uint8_t *track;
    qd_frame_list_t frames = { 0 };
    mz_qd_image_error_t error;

    if ( image_size > UINT32_MAX ) return MZ_QD_IMAGE_ERROR_UNSUPPORTED;

    if ( format == MZ_QD_IMAGE_FORMAT_HXC ) {
        uint32_t tracks;
        uint32_t sides;
        uint32_t encoding;
        if ( image_size < QD_HXC_HEADER_SIZE ) return MZ_QD_IMAGE_ERROR_TRUNCATED;
        tracks = read_le32 ( image + 12u );
        sides = read_le32 ( image + 16u );
        encoding = read_le32 ( image + 20u );
        if ( tracks != 1u || sides != 1u || encoding != 0u ) {
            return MZ_QD_IMAGE_ERROR_UNSUPPORTED;
        }
        descriptor_offset = read_le32 ( image + 36u );
    } else {
        descriptor_offset = QD_DESCRIPTOR_OFFSET_FF;
    }

    if ( descriptor_offset > image_size
      || image_size - descriptor_offset < QD_DESCRIPTOR_SIZE ) {
        return MZ_QD_IMAGE_ERROR_TRUNCATED;
    }
    descriptor = image + descriptor_offset;
    track_offset = read_le32 ( descriptor );
    track_length = read_le32 ( descriptor + 4u );
    window_start = read_le32 ( descriptor + 8u );
    window_end = read_le32 ( descriptor + 12u );

    if ( track_offset < QD_MIN_TRACK_OFFSET || track_length == 0u
      || track_offset > image_size || track_length > image_size - track_offset
      || window_start > window_end || window_end > track_length ) {
        return MZ_QD_IMAGE_ERROR_CORRUPT;
    }
    track = image + track_offset;

    error = find_physical_frames ( track, track_length, &frames );
    if ( error != MZ_QD_IMAGE_OK ) {
        frame_list_free ( &frames );
        return error;
    }
    if ( frames.count == 0 ) {
        error = is_blank_track ( track, track_length )
              ? make_empty_logical ( output, output_size )
              : MZ_QD_IMAGE_ERROR_CORRUPT;
    } else {
        error = build_logical_from_frames ( &frames, output, output_size );
    }
    frame_list_free ( &frames );
    if ( error == MZ_QD_IMAGE_OK && profile != NULL ) {
        memset ( profile, 0, sizeof ( *profile ) );
        profile->format = format;
        profile->container_size = (uint32_t) image_size;
        profile->descriptor_offset = descriptor_offset;
        profile->track_offset = track_offset;
        profile->track_length = track_length;
        profile->stored_track_length = (uint32_t) ( image_size - track_offset );
        profile->window_start = window_start;
        profile->window_end = window_end;
        profile->bit_rate = format == MZ_QD_IMAGE_FORMAT_HXC
                          ? read_le32 ( image + 28u ) : 0u;
        profile->blank_filler = format == MZ_QD_IMAGE_FORMAT_HXC ? 0x01u : 0x11u;
    }
    return error;
}

mz_qd_image_error_t mz_qd_image_decode_with_profile (
                                         const uint8_t *image,
                                         size_t image_size,
                                         uint8_t **logical_image,
                                         size_t *logical_size,
                                         mz_qd_image_profile_t *profile ) {
    mz_qd_image_format_t detected;
    mz_qd_image_error_t error;

    if ( profile != NULL ) memset ( profile, 0, sizeof ( *profile ) );
    if ( logical_image == NULL || logical_size == NULL ) return MZ_QD_IMAGE_ERROR_ARGUMENT;
    *logical_image = NULL;
    *logical_size = 0;
    if ( image == NULL || image_size == 0 ) return MZ_QD_IMAGE_ERROR_ARGUMENT;
    if ( image_size > UINT32_MAX ) return MZ_QD_IMAGE_ERROR_UNSUPPORTED;

    detected = mz_qd_image_detect ( image, image_size );
    if ( profile != NULL ) profile->format = detected;
    if ( detected == MZ_QD_IMAGE_FORMAT_UNKNOWN ) return MZ_QD_IMAGE_ERROR_FORMAT;

    if ( detected == MZ_QD_IMAGE_FORMAT_SHARP_LOGICAL ) {
        uint8_t *copy;
        error = validate_logical ( image, image_size, NULL );
        if ( error != MZ_QD_IMAGE_OK ) return error;
        copy = (uint8_t*) malloc ( image_size );
        if ( copy == NULL ) return MZ_QD_IMAGE_ERROR_MEMORY;
        memcpy ( copy, image, image_size );
        *logical_image = copy;
        *logical_size = image_size;
        if ( profile != NULL ) {
            profile->format = detected;
            profile->container_size = (uint32_t) image_size;
        }
        return MZ_QD_IMAGE_OK;
    }

    return decode_physical ( image, image_size, detected, logical_image, logical_size,
                             profile );
}

mz_qd_image_error_t mz_qd_image_decode ( const uint8_t *image,
                                         size_t image_size,
                                         uint8_t **logical_image,
                                         size_t *logical_size,
                                         mz_qd_image_format_t *format ) {
    mz_qd_image_profile_t profile = { 0 };
    mz_qd_image_error_t error = mz_qd_image_decode_with_profile (
        image, image_size, logical_image, logical_size, &profile );
    if ( format != NULL ) *format = profile.format;
    return error;
}

static mz_qd_image_error_t encode_legacy ( const uint8_t *logical,
                                           size_t logical_size,
                                           size_t target_size,
                                           uint8_t **image,
                                           size_t *image_size ) {
    size_t content_size;
    size_t position;
    uint8_t *output;
    mz_qd_image_error_t error = validate_logical ( logical, logical_size,
                                                   &content_size );
    if ( error != MZ_QD_IMAGE_OK ) return error;
    if ( target_size == 0u ) target_size = QD_LEGACY_IMAGE_SIZE;
    if ( target_size < QD_LEGACY_MIN_TAIL_SIZE
      || content_size > target_size - QD_LEGACY_MIN_TAIL_SIZE ) {
        return MZ_QD_IMAGE_ERROR_CAPACITY;
    }

    output = (uint8_t*) calloc ( target_size, 1u );
    if ( output == NULL ) return MZ_QD_IMAGE_ERROR_MEMORY;
    memcpy ( output, logical, content_size );
    position = content_size;
    output[position++] = 0x00;
    output[position++] = 0x16;
    output[position++] = 0x16;
    output[position++] = 0xa5;
    {
        int write_55 = 1;
        while ( position < target_size - 4u ) {
            output[position++] = write_55 ? 0x55u : 0xaau;
            write_55 = !write_55;
        }
    }
    memcpy ( output + position, s_logical_crc, sizeof ( s_logical_crc ) );
    output[target_size - 1u] = 0x00;
    *image = output;
    *image_size = target_size;
    return MZ_QD_IMAGE_OK;
}

/* Build the canonical QDF byte layout used by qdf2qd and real MZ-800 media.
 * FlashFloppy stores QDF bytes [16, 81936) as an MFM bit-cell stream; the
 * 16-byte QDF signature itself is a file-format marker and is not recorded. */
static mz_qd_image_error_t build_qdf_image ( const uint8_t *logical,
                                             size_t logical_size,
                                             uint8_t **qdf ) {
    size_t logical_position = 8u;
    size_t required_size = QD_QDF_HEADER_SIZE;
    size_t output_position = QD_QDF_HEADER_OFFSET;
    unsigned block_count;
    unsigned file;
    uint8_t *output;
    mz_qd_image_error_t error = validate_logical ( logical, logical_size, NULL );
    if ( error != MZ_QD_IMAGE_OK ) return error;
    block_count = logical[4];

    for ( file = 0; file < block_count / 2u; ++file ) {
        uint16_t body_size = read_le16 ( logical + logical_position + 27u );
        if ( required_size > QD_QDF_IMAGE_SIZE - QD_QDF_FILE_OVERHEAD
          || (size_t) body_size > QD_QDF_IMAGE_SIZE - required_size
                                      - QD_QDF_FILE_OVERHEAD ) {
            return MZ_QD_IMAGE_ERROR_CAPACITY;
        }
        required_size += QD_QDF_FILE_OVERHEAD + (size_t) body_size;
        logical_position += 84u + (size_t) body_size;
    }

    output = (uint8_t*) calloc ( QD_QDF_IMAGE_SIZE, 1u );
    if ( output == NULL ) return MZ_QD_IMAGE_ERROR_MEMORY;
    memcpy ( output, s_qdf_signature, sizeof ( s_qdf_signature ) );

    memset ( output + output_position, 0x16, 9u );
    output_position += 9u;
    {
        uint8_t count_frame[4] = { 0xa5, (uint8_t) block_count, 0, 0 };
        append_crc ( count_frame, 2u );
        memcpy ( output + output_position, count_frame, sizeof ( count_frame ) );
        output_position += sizeof ( count_frame );
    }
    memset ( output + output_position, 0x16, 6u );
    output_position += 6u;
    output_position += 2794u; /* calloc() supplied the required zero gap. */

    logical_position = 8u;
    for ( file = 0; file < block_count / 2u; ++file ) {
        uint8_t header_frame[QD_PHYSICAL_HEADER_SIZE];
        uint16_t body_size = read_le16 ( logical + logical_position + 27u );
        uint8_t *body_frame;

        memset ( output + output_position, 0x16, 10u );
        output_position += 10u;
        memcpy ( header_frame, logical + logical_position + 3u, 68u );
        append_crc ( header_frame, 68u );
        memcpy ( output + output_position, header_frame, sizeof ( header_frame ) );
        output_position += sizeof ( header_frame );
        memset ( output + output_position, 0x16, 7u );
        output_position += 7u + 254u;
        logical_position += 74u;

        body_frame = (uint8_t*) malloc ( (size_t) body_size + 6u );
        if ( body_frame == NULL ) {
            free ( output );
            return MZ_QD_IMAGE_ERROR_MEMORY;
        }
        memcpy ( body_frame, logical + logical_position + 3u,
                 (size_t) body_size + 4u );
        append_crc ( body_frame, (size_t) body_size + 4u );
        memset ( output + output_position, 0x16, 10u );
        output_position += 10u;
        memcpy ( output + output_position, body_frame, (size_t) body_size + 6u );
        output_position += (size_t) body_size + 6u;
        free ( body_frame );
        memset ( output + output_position, 0x16, 7u );
        output_position += 7u + 256u;
        logical_position += (size_t) body_size + 10u;
    }

    *qdf = output;
    return MZ_QD_IMAGE_OK;
}

static uint8_t *mfm_encode ( const uint8_t *data, size_t size ) {
    uint8_t *encoded;
    size_t output_bit = 0;
    int previous = 0;
    size_t i;
    if ( size > (size_t) -1 / 2u ) return NULL;
    encoded = (uint8_t*) calloc ( size * 2u, 1u );
    if ( encoded == NULL ) return NULL;
    for ( i = 0; i < size; ++i ) {
        unsigned bit;
        for ( bit = 0; bit < 8; ++bit ) {
            int current = ( data[i] & ( 1u << bit ) ) != 0;
            int clock = !previous && !current;
            if ( clock ) {
                encoded[output_bit >> 3] |= (uint8_t) ( 1u << ( output_bit & 7u ) );
            }
            ++output_bit;
            if ( current ) {
                encoded[output_bit >> 3] |= (uint8_t) ( 1u << ( output_bit & 7u ) );
            }
            ++output_bit;
            previous = current;
        }
    }
    return encoded;
}

static void copy_bits_lsb_first ( const uint8_t *source,
                                  size_t source_bit_offset,
                                  uint8_t *destination,
                                  size_t destination_bit_offset,
                                  size_t bit_count ) {
    size_t bit;
    for ( bit = 0; bit < bit_count; ++bit ) {
        size_t source_bit = source_bit_offset + bit;
        size_t destination_bit = destination_bit_offset + bit;
        if ( source[source_bit >> 3] & ( 1u << ( source_bit & 7u ) ) ) {
            destination[destination_bit >> 3] |=
                (uint8_t) ( 1u << ( destination_bit & 7u ) );
        }
    }
}

/* HxC output intentionally does not reuse FlashFloppy READY timing.  This
 * layout follows the working MZ-1500 HXCQDDRV Mario reference: count/FNBLK
 * starts 22 bitcells after window_start and the complete track is a
 * continuous MFM-zero carrier rather than a compact stream in 0x01 filler. */
static mz_qd_image_error_t build_hxc_canonical_track (
                                             const uint8_t *logical,
                                             size_t logical_size,
                                             const mz_qd_image_profile_t *profile,
                                             uint8_t **track ) {
    size_t logical_track_size;
    size_t count_position;
    size_t position;
    size_t logical_position = 8u;
    size_t meaningful_end_bits;
    size_t raw_track_bits;
    size_t target_count_bit;
    unsigned block_count;
    unsigned file;
    uint8_t *logical_track;
    uint8_t *unshifted;
    uint8_t *output;
    mz_qd_image_error_t error = validate_logical ( logical, logical_size, NULL );
    if ( error != MZ_QD_IMAGE_OK ) return error;

    if ( ( profile->stored_track_length & 1u ) != 0u
      || ( profile->window_start & 1u ) != 0u ) {
        return MZ_QD_IMAGE_ERROR_UNSUPPORTED;
    }
    if ( profile->stored_track_length > (size_t) -1 / 8u
      || profile->window_start > (size_t) -1 / 8u
      || profile->window_end > (size_t) -1 / 8u ) {
        return MZ_QD_IMAGE_ERROR_UNSUPPORTED;
    }

    raw_track_bits = (size_t) profile->stored_track_length * 8u;
    logical_track_size = (size_t) profile->stored_track_length / 2u;
    target_count_bit = (size_t) profile->window_start * 8u
                     + QD_HXC_MARIO_FNBLK_READY_OFFSET_BITS;
    if ( target_count_bit < QD_HXC_MFM_SHIFT_BITS + 1u
      || ( target_count_bit - QD_HXC_MFM_SHIFT_BITS - 1u ) % 16u != 0u ) {
        return MZ_QD_IMAGE_ERROR_UNSUPPORTED;
    }
    count_position = ( target_count_bit - QD_HXC_MFM_SHIFT_BITS - 1u ) / 16u;
    if ( count_position < QD_HXC_COUNT_PRE_SYNC
      || count_position > logical_track_size ) {
        return MZ_QD_IMAGE_ERROR_CAPACITY;
    }

    position = count_position + 4u + QD_HXC_COUNT_POST_SYNC
             + QD_HXC_COUNT_HEADER_GAP;
    block_count = logical[4];
    for ( file = 0; file < block_count / 2u; ++file ) {
        uint16_t body_size = read_le16 ( logical + logical_position + 27u );
        size_t file_size = QD_HXC_BLOCK_PRE_SYNC + QD_PHYSICAL_HEADER_SIZE
                         + QD_HXC_BLOCK_POST_SYNC + QD_HXC_HEADER_BODY_GAP
                         + QD_HXC_BLOCK_PRE_SYNC + (size_t) body_size + 6u
                         + QD_HXC_BLOCK_POST_SYNC + QD_HXC_BODY_HEADER_GAP;
        if ( position > (size_t) -1 - file_size ) {
            return MZ_QD_IMAGE_ERROR_MEMORY;
        }
        position += file_size;
        logical_position += 84u + (size_t) body_size;
    }
    if ( position > ( (size_t) -1 - QD_HXC_MFM_SHIFT_BITS ) / 16u ) {
        return MZ_QD_IMAGE_ERROR_MEMORY;
    }
    meaningful_end_bits = QD_HXC_MFM_SHIFT_BITS + position * 16u;
    if ( position > logical_track_size
      || meaningful_end_bits > (size_t) profile->window_end * 8u
      || meaningful_end_bits > raw_track_bits ) {
        return MZ_QD_IMAGE_ERROR_CAPACITY;
    }

    logical_track = (uint8_t*) calloc ( logical_track_size, 1u );
    if ( logical_track == NULL ) return MZ_QD_IMAGE_ERROR_MEMORY;
    position = count_position - QD_HXC_COUNT_PRE_SYNC;
    memset ( logical_track + position, 0x16, QD_HXC_COUNT_PRE_SYNC );
    position += QD_HXC_COUNT_PRE_SYNC;
    logical_track[position] = 0xa5u;
    logical_track[position + 1u] = (uint8_t) block_count;
    append_crc ( logical_track + position, 2u );
    position += 4u;
    memset ( logical_track + position, 0x16, QD_HXC_COUNT_POST_SYNC );
    position += QD_HXC_COUNT_POST_SYNC + QD_HXC_COUNT_HEADER_GAP;

    logical_position = 8u;
    for ( file = 0; file < block_count / 2u; ++file ) {
        uint16_t body_size = read_le16 ( logical + logical_position + 27u );

        memset ( logical_track + position, 0x16, QD_HXC_BLOCK_PRE_SYNC );
        position += QD_HXC_BLOCK_PRE_SYNC;
        memcpy ( logical_track + position, logical + logical_position + 3u, 68u );
        append_crc ( logical_track + position, 68u );
        position += QD_PHYSICAL_HEADER_SIZE;
        memset ( logical_track + position, 0x16, QD_HXC_BLOCK_POST_SYNC );
        position += QD_HXC_BLOCK_POST_SYNC + QD_HXC_HEADER_BODY_GAP;
        logical_position += 74u;

        memset ( logical_track + position, 0x16, QD_HXC_BLOCK_PRE_SYNC );
        position += QD_HXC_BLOCK_PRE_SYNC;
        memcpy ( logical_track + position, logical + logical_position + 3u,
                 (size_t) body_size + 4u );
        append_crc ( logical_track + position, (size_t) body_size + 4u );
        position += (size_t) body_size + 6u;
        memset ( logical_track + position, 0x16, QD_HXC_BLOCK_POST_SYNC );
        position += QD_HXC_BLOCK_POST_SYNC + QD_HXC_BODY_HEADER_GAP;
        logical_position += (size_t) body_size + 10u;
    }

    unshifted = mfm_encode ( logical_track, logical_track_size );
    free ( logical_track );
    if ( unshifted == NULL ) return MZ_QD_IMAGE_ERROR_MEMORY;
    output = (uint8_t*) calloc ( profile->stored_track_length, 1u );
    if ( output == NULL ) {
        free ( unshifted );
        return MZ_QD_IMAGE_ERROR_MEMORY;
    }

    /* Continue the zero-carrier phase across the five virtual cells before
     * the stored track, then shift the encoded stream LSB-first. */
    output[0] = 0x0au;
    copy_bits_lsb_first ( unshifted, 0u, output, QD_HXC_MFM_SHIFT_BITS,
                          raw_track_bits - QD_HXC_MFM_SHIFT_BITS );
    free ( unshifted );
    *track = output;
    return MZ_QD_IMAGE_OK;
}

static mz_qd_image_error_t encode_physical ( const uint8_t *logical,
                                             size_t logical_size,
                                             const mz_qd_image_profile_t *profile,
                                             uint8_t **image,
                                             size_t *image_size ) {
    uint8_t *raw_track = NULL;
    uint8_t *output;
    uint8_t *track;
    mz_qd_image_error_t error;

    if ( profile->container_size == 0u
      || profile->descriptor_offset > profile->container_size
      || profile->container_size - profile->descriptor_offset < QD_DESCRIPTOR_SIZE
      || profile->track_offset < QD_MIN_TRACK_OFFSET
      || profile->track_offset > profile->container_size
      || profile->stored_track_length > profile->container_size - profile->track_offset
      || profile->track_length == 0u
      || profile->track_length > profile->stored_track_length
      || profile->window_start > profile->window_end
      || profile->window_end > profile->track_length ) {
        return MZ_QD_IMAGE_ERROR_CORRUPT;
    }

    if ( profile->format == MZ_QD_IMAGE_FORMAT_FLASHFLOPPY ) {
        uint8_t *qdf = NULL;
        uint8_t *encoded;
        size_t encoded_size = ( QD_QDF_IMAGE_SIZE - QD_QDF_SIGNATURE_SIZE ) * 2u;
        error = build_qdf_image ( logical, logical_size, &qdf );
        if ( error != MZ_QD_IMAGE_OK ) return error;
        if ( profile->window_start > profile->window_end
          || encoded_size > (size_t) profile->window_end - profile->window_start
          || encoded_size > (size_t) profile->stored_track_length
                           - profile->window_start ) {
            free ( qdf );
            return MZ_QD_IMAGE_ERROR_CAPACITY;
        }
        encoded = mfm_encode ( qdf + QD_QDF_SIGNATURE_SIZE,
                               QD_QDF_IMAGE_SIZE - QD_QDF_SIGNATURE_SIZE );
        free ( qdf );
        if ( encoded == NULL ) return MZ_QD_IMAGE_ERROR_MEMORY;
        raw_track = (uint8_t*) malloc ( profile->stored_track_length );
        if ( raw_track == NULL ) {
            free ( encoded );
            return MZ_QD_IMAGE_ERROR_MEMORY;
        }
        memset ( raw_track, profile->blank_filler, profile->stored_track_length );
        /* FlashFloppy uses the QDF-compatible payload at window_start.  Its
         * first count/FNBLK is then about 380.4 ms after READY, matching the
         * Sharp ROM search window and real formatted media (~379.2 ms). */
        memcpy ( raw_track + profile->window_start, encoded, encoded_size );
        free ( encoded );
    } else if ( profile->format == MZ_QD_IMAGE_FORMAT_HXC ) {
        error = build_hxc_canonical_track ( logical, logical_size, profile,
                                            &raw_track );
        if ( error != MZ_QD_IMAGE_OK ) return error;
    } else {
        return MZ_QD_IMAGE_ERROR_ARGUMENT;
    }

    output = (uint8_t*) calloc ( profile->container_size, 1u );
    if ( output == NULL ) {
        free ( raw_track );
        return MZ_QD_IMAGE_ERROR_MEMORY;
    }
    if ( profile->format == MZ_QD_IMAGE_FORMAT_HXC ) {
        memcpy ( output, s_hxc_signature, sizeof ( s_hxc_signature ) );
        write_le32 ( output + 8u, 0u );
        write_le32 ( output + 12u, 1u );
        write_le32 ( output + 16u, 1u );
        write_le32 ( output + 20u, 0u );
        write_le32 ( output + 24u, 0u );
        write_le32 ( output + 28u, profile->bit_rate );
        write_le32 ( output + 32u, 0u );
        write_le32 ( output + 36u, profile->descriptor_offset );
    } else if ( profile->format == MZ_QD_IMAGE_FORMAT_FLASHFLOPPY ) {
        output[3] = 'Q';
        output[4] = 'D';
    }

    write_le32 ( output + profile->descriptor_offset, profile->track_offset );
    write_le32 ( output + profile->descriptor_offset + 4u, profile->track_length );
    write_le32 ( output + profile->descriptor_offset + 8u, profile->window_start );
    write_le32 ( output + profile->descriptor_offset + 12u, profile->window_end );
    track = output + profile->track_offset;
    memcpy ( track, raw_track, profile->stored_track_length );
    free ( raw_track );
    *image = output;
    *image_size = profile->container_size;
    return MZ_QD_IMAGE_OK;
}

mz_qd_image_error_t mz_qd_image_encode ( const uint8_t *logical_image,
                                         size_t logical_size,
                                         const mz_qd_image_profile_t *profile,
                                         uint8_t **image,
                                         size_t *image_size ) {
    if ( image == NULL || image_size == NULL || profile == NULL ) {
        return MZ_QD_IMAGE_ERROR_ARGUMENT;
    }
    *image = NULL;
    *image_size = 0;
    if ( logical_image == NULL || logical_size == 0u ) {
        return MZ_QD_IMAGE_ERROR_ARGUMENT;
    }
    if ( profile->format == MZ_QD_IMAGE_FORMAT_SHARP_LOGICAL ) {
        return encode_legacy ( logical_image, logical_size,
                               profile->container_size, image, image_size );
    }
    if ( profile->format == MZ_QD_IMAGE_FORMAT_HXC
      || profile->format == MZ_QD_IMAGE_FORMAT_FLASHFLOPPY ) {
        return encode_physical ( logical_image, logical_size, profile,
                                 image, image_size );
    }
    return MZ_QD_IMAGE_ERROR_ARGUMENT;
}

const char *mz_qd_image_error_string ( mz_qd_image_error_t error ) {
    switch ( error ) {
        case MZ_QD_IMAGE_OK:                return "no error";
        case MZ_QD_IMAGE_ERROR_ARGUMENT:    return "invalid codec argument";
        case MZ_QD_IMAGE_ERROR_FORMAT:      return "unknown .qd format";
        case MZ_QD_IMAGE_ERROR_TRUNCATED:   return "truncated .qd image";
        case MZ_QD_IMAGE_ERROR_UNSUPPORTED: return "unsupported Quick Disk geometry or encoding";
        case MZ_QD_IMAGE_ERROR_CORRUPT:     return "corrupt Quick Disk container, frame, or CRC";
        case MZ_QD_IMAGE_ERROR_SEQUENCE:    return "inconsistent Quick Disk block sequence";
        case MZ_QD_IMAGE_ERROR_CAPACITY:    return "Quick Disk image capacity exceeded";
        case MZ_QD_IMAGE_ERROR_MEMORY:      return "not enough memory to process .qd image";
        default:                            return "unknown .qd codec error";
    }
}
