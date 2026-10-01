#include "mztest.h"

#include <stdlib.h>
#include <string.h>

#include "libs/qd_image/qd_image.h"

void setUp ( void ) {
}

void tearDown ( void ) {
}

static void write_le16 ( uint8_t *p, uint16_t value ) {
    p[0] = (uint8_t) value;
    p[1] = (uint8_t) ( value >> 8 );
}

static void write_le32 ( uint8_t *p, uint32_t value ) {
    p[0] = (uint8_t) value;
    p[1] = (uint8_t) ( value >> 8 );
    p[2] = (uint8_t) ( value >> 16 );
    p[3] = (uint8_t) ( value >> 24 );
}

static uint32_t read_le32 ( const uint8_t *p ) {
    return (uint32_t) p[0]
         | ( (uint32_t) p[1] << 8 )
         | ( (uint32_t) p[2] << 16 )
         | ( (uint32_t) p[3] << 24 );
}

static uint8_t reverse_bits ( uint8_t value ) {
    value = (uint8_t) ( ( ( value & 0x55u ) << 1 ) | ( ( value >> 1 ) & 0x55u ) );
    value = (uint8_t) ( ( ( value & 0x33u ) << 2 ) | ( ( value >> 2 ) & 0x33u ) );
    return (uint8_t) ( ( value << 4 ) | ( value >> 4 ) );
}

static uint16_t crc_update ( uint16_t crc, uint8_t data ) {
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

static void add_crc ( uint8_t *frame, size_t size_without_crc ) {
    uint16_t crc = 0;
    size_t i;
    for ( i = 0; i < size_without_crc; ++i ) crc = crc_update ( crc, frame[i] );
    frame[size_without_crc] = reverse_bits ( (uint8_t) ( crc >> 8 ) );
    frame[size_without_crc + 1u] = reverse_bits ( (uint8_t) crc );
}

static void append_framed ( uint8_t *stream,
                            size_t *position,
                            const uint8_t *frame,
                            size_t frame_size ) {
    unsigned i;
    stream[(*position)++] = 0x00;
    for ( i = 0; i < 10; ++i ) stream[(*position)++] = 0x16;
    memcpy ( stream + *position, frame, frame_size );
    *position += frame_size;
    for ( i = 0; i < 8; ++i ) stream[(*position)++] = 0x00;
}

static uint8_t *mfm_encode ( const uint8_t *data, size_t size, size_t *encoded_size ) {
    uint8_t *encoded = (uint8_t*) calloc ( size * 2u, 1u );
    size_t output_bit = 0;
    int previous = 0;
    size_t i;
    TEST_ASSERT_NOT_NULL ( encoded );
    for ( i = 0; i < size; ++i ) {
        unsigned bit;
        for ( bit = 0; bit < 8; ++bit ) {
            int current = ( data[i] & ( 1u << bit ) ) != 0;
            int clock = !previous && !current;
            if ( clock ) encoded[output_bit >> 3] |= (uint8_t) ( 1u << ( output_bit & 7u ) );
            ++output_bit;
            if ( current ) encoded[output_bit >> 3] |= (uint8_t) ( 1u << ( output_bit & 7u ) );
            ++output_bit;
            previous = current;
        }
    }
    *encoded_size = size * 2u;
    return encoded;
}

static uint8_t mfm_decode_aligned_byte ( const uint8_t *encoded,
                                         size_t byte_index ) {
    size_t first_data_cell = byte_index * 16u + 1u;
    uint8_t value = 0;
    unsigned bit;
    for ( bit = 0; bit < 8u; ++bit ) {
        size_t position = first_data_cell + bit * 2u;
        if ( encoded[position >> 3] & ( 1u << ( position & 7u ) ) ) {
            value |= (uint8_t) ( 1u << bit );
        }
    }
    return value;
}

static uint8_t mfm_decode_byte_at_data_bit ( const uint8_t *encoded,
                                             size_t first_data_cell ) {
    uint8_t value = 0;
    unsigned bit;
    for ( bit = 0; bit < 8u; ++bit ) {
        size_t position = first_data_cell + bit * 2u;
        if ( encoded[position >> 3] & ( 1u << ( position & 7u ) ) ) {
            value |= (uint8_t) ( 1u << bit );
        }
    }
    return value;
}

static uint8_t *make_container ( mz_qd_image_format_t format,
                                 const uint8_t *track,
                                 size_t track_size,
                                 size_t *image_size ) {
    const size_t track_offset = 0x400u;
    uint8_t *image;
    *image_size = track_offset + track_size;
    image = (uint8_t*) calloc ( *image_size, 1u );
    TEST_ASSERT_NOT_NULL ( image );

    if ( format == MZ_QD_IMAGE_FORMAT_HXC ) {
        memcpy ( image, "HXCQDDRV", 8u );
        write_le32 ( image + 12u, 1u );
        write_le32 ( image + 16u, 1u );
        write_le32 ( image + 20u, 0u );
        write_le32 ( image + 36u, 0x200u );
    } else {
        image[3] = 'Q';
        image[4] = 'D';
    }
    write_le32 ( image + 0x200u, (uint32_t) track_offset );
    write_le32 ( image + 0x204u, (uint32_t) track_size );
    write_le32 ( image + 0x208u, 0u );
    write_le32 ( image + 0x20cu, (uint32_t) track_size );
    memcpy ( image + track_offset, track, track_size );
    return image;
}

static uint8_t *make_one_file_hxc ( size_t *image_size ) {
    uint8_t stream[256] = { 0 };
    uint8_t count[4] = { 0xa5, 0x02, 0, 0 };
    uint8_t header[70] = { 0 };
    uint8_t body[9] = { 0xa5, 0x05, 0x03, 0x00, 0xde, 0xad, 0xbe, 0, 0 };
    size_t stream_size = 0;
    size_t track_size;
    uint8_t *track;
    uint8_t *image;

    add_crc ( count, 2u );
    header[0] = 0xa5;
    header[1] = 0x00;
    write_le16 ( header + 2u, 64u );
    header[4] = 0x01;
    memcpy ( header + 5u, "TEST", 4u );
    header[21] = 0x0d;
    write_le16 ( header + 24u, 3u );
    write_le16 ( header + 26u, 0x1200u );
    write_le16 ( header + 28u, 0x1200u );
    add_crc ( header, 68u );
    add_crc ( body, 7u );

    append_framed ( stream, &stream_size, count, sizeof ( count ) );
    append_framed ( stream, &stream_size, header, sizeof ( header ) );
    append_framed ( stream, &stream_size, body, sizeof ( body ) );
    track = mfm_encode ( stream, stream_size, &track_size );
    image = make_container ( MZ_QD_IMAGE_FORMAT_HXC, track, track_size, image_size );
    free ( track );
    return image;
}

static uint8_t *make_two_file_logical ( size_t *logical_size ) {
    size_t source_size;
    uint8_t *source = make_one_file_hxc ( &source_size );
    uint8_t *one_file = NULL;
    size_t one_file_size = 0;
    uint8_t *two_file;

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_decode ( source, source_size, &one_file, &one_file_size, NULL ) );
    TEST_ASSERT_EQUAL_size_t ( 95u, one_file_size );
    two_file = (uint8_t*) malloc ( 8u + 2u * ( one_file_size - 8u ) );
    TEST_ASSERT_NOT_NULL ( two_file );
    memcpy ( two_file, one_file, one_file_size );
    memcpy ( two_file + one_file_size, one_file + 8u, one_file_size - 8u );
    two_file[4] = 4u;
    memcpy ( two_file + one_file_size + 8u, "NEXT", 4u );
    *logical_size = 8u + 2u * ( one_file_size - 8u );
    free ( one_file );
    free ( source );
    return two_file;
}

static void test_detects_supported_variants ( void ) {
    const uint8_t logical[8] = { 0x00, 0x16, 0x16, 0xa5, 0x00, 'C', 'R', 'C' };
    const uint8_t hxc[8] = { 'H', 'X', 'C', 'Q', 'D', 'D', 'R', 'V' };
    const uint8_t flash[5] = { 0, 0, 0, 'Q', 'D' };
    const uint8_t unknown[8] = { 0 };

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_FORMAT_SHARP_LOGICAL,
                            mz_qd_image_detect ( logical, sizeof ( logical ) ) );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_FORMAT_HXC,
                            mz_qd_image_detect ( hxc, sizeof ( hxc ) ) );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_FORMAT_FLASHFLOPPY,
                            mz_qd_image_detect ( flash, sizeof ( flash ) ) );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_FORMAT_UNKNOWN,
                            mz_qd_image_detect ( unknown, sizeof ( unknown ) ) );
}

static void test_logical_image_is_validated_and_copied ( void ) {
    const uint8_t logical[8] = { 0x00, 0x16, 0x16, 0xa5, 0x00, 'C', 'R', 'C' };
    uint8_t *output = NULL;
    size_t output_size = 0;
    mz_qd_image_format_t format = MZ_QD_IMAGE_FORMAT_UNKNOWN;

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_decode ( logical, sizeof ( logical ), &output, &output_size, &format ) );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_FORMAT_SHARP_LOGICAL, format );
    TEST_ASSERT_EQUAL_size_t ( sizeof ( logical ), output_size );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( logical, output, sizeof ( logical ) );
    TEST_ASSERT_NOT_EQUAL ( logical, output );
    free ( output );
}

static void test_odd_logical_block_count_is_rejected ( void ) {
    const uint8_t logical[8] = { 0x00, 0x16, 0x16, 0xa5, 0x01, 'C', 'R', 'C' };
    uint8_t *output = NULL;
    size_t output_size = 0;
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_ERROR_SEQUENCE,
        mz_qd_image_decode ( logical, sizeof ( logical ), &output, &output_size, NULL ) );
    TEST_ASSERT_NULL ( output );
    TEST_ASSERT_EQUAL_size_t ( 0, output_size );
}

static void test_blank_physical_variants_become_empty_logical_image ( void ) {
    mz_qd_image_format_t variants[2] = {
        MZ_QD_IMAGE_FORMAT_HXC, MZ_QD_IMAGE_FORMAT_FLASHFLOPPY
    };
    unsigned v;
    for ( v = 0; v < 2; ++v ) {
        uint8_t track[128];
        size_t image_size;
        uint8_t *image;
        uint8_t *output = NULL;
        size_t output_size = 0;
        mz_qd_image_format_t detected;
        memset ( track, variants[v] == MZ_QD_IMAGE_FORMAT_HXC ? 0x01 : 0x11,
                 sizeof ( track ) );
        image = make_container ( variants[v], track, sizeof ( track ), &image_size );
        TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
            mz_qd_image_decode ( image, image_size, &output, &output_size, &detected ) );
        TEST_ASSERT_EQUAL_INT ( variants[v], detected );
        TEST_ASSERT_EQUAL_size_t ( 8, output_size );
        TEST_ASSERT_EQUAL_HEX8 ( 0xa5, output[3] );
        TEST_ASSERT_EQUAL_UINT8 ( 0, output[4] );
        free ( output );
        free ( image );
    }
}

static void test_hxc_mfm_frames_decode_to_mzq_stream ( void ) {
    size_t image_size;
    uint8_t *image = make_one_file_hxc ( &image_size );
    uint8_t *output = NULL;
    size_t output_size = 0;
    mz_qd_image_format_t format;

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_decode ( image, image_size, &output, &output_size, &format ) );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_FORMAT_HXC, format );
    TEST_ASSERT_EQUAL_size_t ( 95, output_size );
    TEST_ASSERT_EQUAL_UINT8 ( 2, output[4] );
    TEST_ASSERT_EQUAL_UINT8 ( 0x01, output[15] );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( "TEST", output + 16u, 4u );
    TEST_ASSERT_EQUAL_HEX8 ( 0x03, output[35] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x05, output[86] );
    TEST_ASSERT_EQUAL_HEX8 ( 0xde, output[89] );
    TEST_ASSERT_EQUAL_HEX8 ( 0xad, output[90] );
    TEST_ASSERT_EQUAL_HEX8 ( 0xbe, output[91] );

    /* The converted stream must itself be a fully valid logical .qd image. */
    {
        uint8_t *second = NULL;
        size_t second_size = 0;
        mz_qd_image_format_t second_format;
        TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
            mz_qd_image_decode ( output, output_size, &second, &second_size, &second_format ) );
        TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_FORMAT_SHARP_LOGICAL, second_format );
        TEST_ASSERT_EQUAL_size_t ( output_size, second_size );
        TEST_ASSERT_EQUAL_UINT8_ARRAY ( output, second, output_size );
        free ( second );
    }

    free ( output );
    free ( image );
}

static void test_bad_physical_frame_crc_is_rejected ( void ) {
    size_t image_size;
    uint8_t *image = make_one_file_hxc ( &image_size );
    uint8_t *output = NULL;
    size_t output_size = 0;
    /* Decoded stream byte 102 is inside the header CRC.  MFM data cells are
     * the odd-numbered cells, LSB first. */
    size_t bit_position = 102u * 16u + 1u;
    image[0x400u + ( bit_position >> 3 )] ^= (uint8_t) ( 1u << ( bit_position & 7u ) );

    TEST_ASSERT_NOT_EQUAL ( MZ_QD_IMAGE_OK,
        mz_qd_image_decode ( image, image_size, &output, &output_size, NULL ) );
    TEST_ASSERT_NULL ( output );
    TEST_ASSERT_EQUAL_size_t ( 0, output_size );
    free ( image );
}

static void test_unsupported_hxc_geometry_is_rejected ( void ) {
    uint8_t track[32];
    size_t image_size;
    uint8_t *image;
    uint8_t *output = NULL;
    size_t output_size = 0;
    memset ( track, 0x01, sizeof ( track ) );
    image = make_container ( MZ_QD_IMAGE_FORMAT_HXC, track, sizeof ( track ), &image_size );
    write_le32 ( image + 12u, 2u );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_ERROR_UNSUPPORTED,
        mz_qd_image_decode ( image, image_size, &output, &output_size, NULL ) );
    free ( image );
}

static void test_invalid_descriptor_is_rejected ( void ) {
    uint8_t track[32];
    size_t image_size;
    uint8_t *image;
    uint8_t *output = NULL;
    size_t output_size = 0;
    memset ( track, 0x01, sizeof ( track ) );
    image = make_container ( MZ_QD_IMAGE_FORMAT_HXC, track, sizeof ( track ), &image_size );
    write_le32 ( image + 0x200u, 0x3ffu );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_ERROR_CORRUPT,
        mz_qd_image_decode ( image, image_size, &output, &output_size, NULL ) );
    free ( image );
}

static mz_qd_image_profile_t make_writable_profile ( mz_qd_image_format_t format ) {
    mz_qd_image_profile_t profile;
    memset ( &profile, 0, sizeof ( profile ) );
    profile.format = format;
    profile.container_size = 0x2400u;
    profile.descriptor_offset = 0x200u;
    profile.track_offset = 0x400u;
    profile.track_length = 0x2000u;
    profile.stored_track_length = 0x2000u;
    profile.window_start = 0x100u;
    profile.window_end = 0x1f00u;
    profile.bit_rate = format == MZ_QD_IMAGE_FORMAT_HXC ? 203388u : 0u;
    profile.blank_filler = format == MZ_QD_IMAGE_FORMAT_HXC ? 0x01u : 0x11u;
    return profile;
}

static void test_physical_encode_round_trip ( void ) {
    size_t source_size;
    uint8_t *source = make_one_file_hxc ( &source_size );
    uint8_t *logical = NULL;
    size_t logical_size = 0;
    mz_qd_image_format_t variants[2] = {
        MZ_QD_IMAGE_FORMAT_HXC, MZ_QD_IMAGE_FORMAT_FLASHFLOPPY
    };
    unsigned v;

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_decode ( source, source_size, &logical, &logical_size, NULL ) );
    for ( v = 0; v < 2; ++v ) {
        mz_qd_image_profile_t requested;
        mz_qd_image_profile_t loaded;
        uint8_t *encoded = NULL;
        size_t encoded_size = 0;
        uint8_t *decoded = NULL;
        size_t decoded_size = 0;

        if ( variants[v] == MZ_QD_IMAGE_FORMAT_FLASHFLOPPY ) {
            TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
                mz_qd_image_profile_init_default ( &requested, variants[v] ) );
        } else {
            requested = make_writable_profile ( variants[v] );
        }
        TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
            mz_qd_image_encode ( logical, logical_size, &requested,
                                 &encoded, &encoded_size ) );
        TEST_ASSERT_EQUAL_size_t ( requested.container_size, encoded_size );
        TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
            mz_qd_image_decode_with_profile ( encoded, encoded_size,
                                              &decoded, &decoded_size, &loaded ) );
        TEST_ASSERT_EQUAL_INT ( variants[v], loaded.format );
        TEST_ASSERT_EQUAL_UINT32 ( requested.container_size, loaded.container_size );
        TEST_ASSERT_EQUAL_UINT32 ( requested.descriptor_offset, loaded.descriptor_offset );
        TEST_ASSERT_EQUAL_UINT32 ( requested.track_offset, loaded.track_offset );
        TEST_ASSERT_EQUAL_UINT32 ( requested.track_length, loaded.track_length );
        TEST_ASSERT_EQUAL_UINT32 ( requested.window_start, loaded.window_start );
        TEST_ASSERT_EQUAL_UINT32 ( requested.window_end, loaded.window_end );
        TEST_ASSERT_EQUAL_UINT32 ( requested.bit_rate, loaded.bit_rate );
        TEST_ASSERT_EQUAL_size_t ( logical_size, decoded_size );
        TEST_ASSERT_EQUAL_UINT8_ARRAY ( logical, decoded, logical_size );
        free ( decoded );
        free ( encoded );
    }
    free ( logical );
    free ( source );
}

static void test_flashfloppy_encode_uses_real_mz_timing_placement ( void ) {
    const uint8_t logical[8] = { 0x00, 0x16, 0x16, 0xa5, 0x00, 'C', 'R', 'C' };
    mz_qd_image_profile_t profile;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    size_t i;
    const uint8_t *mfm;
    size_t qdf_header = 0x12eau - 16u;

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_profile_init_default ( &profile,
                                           MZ_QD_IMAGE_FORMAT_FLASHFLOPPY ) );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_encode ( logical, sizeof ( logical ), &profile,
                             &encoded, &encoded_size ) );

    TEST_ASSERT_EQUAL_size_t ( 0x32000u, encoded_size );
    TEST_ASSERT_EQUAL_UINT8 ( 'Q', encoded[3] );
    TEST_ASSERT_EQUAL_UINT8 ( 'D', encoded[4] );
    TEST_ASSERT_EQUAL_UINT32 ( 0x400u, read_le32 ( encoded + 0x200u ) );
    TEST_ASSERT_EQUAL_UINT32 ( 0x31a99u, read_le32 ( encoded + 0x204u ) );
    TEST_ASSERT_EQUAL_UINT32 ( 0x31a9u, read_le32 ( encoded + 0x208u ) );
    TEST_ASSERT_EQUAL_UINT32 ( 0x2b745u, read_le32 ( encoded + 0x20cu ) );
    for ( i = 0x400u; i < 0x35a9u; ++i ) {
        TEST_ASSERT_EQUAL_UINT8 ( 0x11u, encoded[i] );
    }
    TEST_ASSERT_EQUAL_UINT8 ( 0x55u, encoded[0x35a9u] );
    TEST_ASSERT_EQUAL_UINT8 ( 0x11u, encoded[0x35a9u + 0x28000u] );
    mfm = encoded + 0x35a9u;
    TEST_ASSERT_EQUAL_UINT8 ( 0x00u, mfm_decode_aligned_byte ( mfm, 0u ) );
    for ( i = 0; i < 9u; ++i ) {
        TEST_ASSERT_EQUAL_UINT8 ( 0x16u,
            mfm_decode_aligned_byte ( mfm, qdf_header + i ) );
    }
    TEST_ASSERT_EQUAL_UINT8 ( 0xa5u,
        mfm_decode_aligned_byte ( mfm, qdf_header + 9u ) );
    TEST_ASSERT_EQUAL_UINT8 ( 0x00u,
        mfm_decode_aligned_byte ( mfm, qdf_header + 10u ) );
    /* QDF payload starts at track window_start 0x31a9.  Its count A5 data
     * cell is 0x25c6 raw bytes + bit 1 later: track 0x576f bit 1, about
     * 380.384 ms after READY (real MZ-formatted media is about 379.214 ms). */
    TEST_ASSERT_EQUAL_UINT8 ( 0xa5u,
        mfm_decode_byte_at_data_bit ( encoded + 0x400u,
                                     0x576fu * 8u + 1u ) );
    free ( encoded );
}

static void test_hxc_encode_matches_mario_reference_layout ( void ) {
    size_t source_size;
    uint8_t *source = make_one_file_hxc ( &source_size );
    uint8_t *logical = NULL;
    size_t logical_size = 0;
    mz_qd_image_profile_t profile;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    const uint8_t *track;
    size_t count_bit = 0x3202u * 8u + 6u;
    size_t header_bit = 0x478au * 8u + 6u;
    size_t body_bit = 0x4a34u * 8u + 6u;
    size_t i;

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_decode ( source, source_size, &logical, &logical_size, NULL ) );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_profile_init_default ( &profile, MZ_QD_IMAGE_FORMAT_HXC ) );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_encode ( logical, logical_size, &profile,
                             &encoded, &encoded_size ) );

    TEST_ASSERT_EQUAL_size_t ( 0x32000u, encoded_size );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( "HXCQDDRV", encoded, 8u );
    TEST_ASSERT_EQUAL_UINT32 ( 203388u, read_le32 ( encoded + 28u ) );
    TEST_ASSERT_EQUAL_UINT32 ( 0x400u, read_le32 ( encoded + 0x200u ) );
    TEST_ASSERT_EQUAL_UINT32 ( 0x31c00u, read_le32 ( encoded + 0x204u ) );
    TEST_ASSERT_EQUAL_UINT32 ( 0x3200u, read_le32 ( encoded + 0x208u ) );
    TEST_ASSERT_EQUAL_UINT32 ( 0x25600u, read_le32 ( encoded + 0x20cu ) );

    track = encoded + 0x400u;
    TEST_ASSERT_EQUAL_UINT8 ( 0xa5u,
        mfm_decode_byte_at_data_bit ( track, count_bit ) );
    TEST_ASSERT_EQUAL_UINT8 ( 0x02u,
        mfm_decode_byte_at_data_bit ( track, count_bit + 16u ) );
    TEST_ASSERT_EQUAL_UINT8 ( 0xa5u,
        mfm_decode_byte_at_data_bit ( track, header_bit ) );
    TEST_ASSERT_EQUAL_UINT8 ( 0xa5u,
        mfm_decode_byte_at_data_bit ( track, body_bit ) );
    TEST_ASSERT_EQUAL_size_t ( 5512u * 8u, header_bit - count_bit );
    TEST_ASSERT_EQUAL_size_t ( 682u * 8u, body_bit - header_bit );
    for ( i = 0; i < 9u; ++i ) {
        TEST_ASSERT_EQUAL_UINT8 ( 0x16u,
            mfm_decode_byte_at_data_bit ( track, count_bit - ( 9u - i ) * 16u ) );
    }
    for ( i = 0x3400u; i < 0x3500u; ++i ) {
        TEST_ASSERT_EQUAL_UINT8 ( 0xaau, track[i] );
    }

    free ( encoded );
    free ( logical );
    free ( source );
}

static void test_two_file_physical_round_trip ( void ) {
    const mz_qd_image_format_t formats[2] = {
        MZ_QD_IMAGE_FORMAT_HXC, MZ_QD_IMAGE_FORMAT_FLASHFLOPPY
    };
    size_t logical_size;
    uint8_t *logical = make_two_file_logical ( &logical_size );
    unsigned i;

    for ( i = 0; i < 2u; ++i ) {
        mz_qd_image_profile_t profile;
        uint8_t *encoded = NULL;
        size_t encoded_size = 0;
        uint8_t *decoded = NULL;
        size_t decoded_size = 0;
        TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
            mz_qd_image_profile_init_default ( &profile, formats[i] ) );
        TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
            mz_qd_image_encode ( logical, logical_size, &profile,
                                 &encoded, &encoded_size ) );
        TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
            mz_qd_image_decode ( encoded, encoded_size, &decoded,
                                 &decoded_size, NULL ) );
        TEST_ASSERT_EQUAL_size_t ( logical_size, decoded_size );
        TEST_ASSERT_EQUAL_UINT8_ARRAY ( logical, decoded, logical_size );
        TEST_ASSERT_EQUAL_UINT8 ( 4u, decoded[4] );
        TEST_ASSERT_EQUAL_UINT8_ARRAY ( "TEST", decoded + 16u, 4u );
        TEST_ASSERT_EQUAL_UINT8_ARRAY ( "NEXT", decoded + 103u, 4u );
        free ( decoded );
        free ( encoded );
    }
    free ( logical );
}

static void test_reader_accepts_historical_qdf2qd_placement ( void ) {
    size_t source_size;
    uint8_t *source = make_one_file_hxc ( &source_size );
    uint8_t *logical = NULL;
    size_t logical_size = 0;
    mz_qd_image_profile_t profile;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    uint8_t *decoded = NULL;
    size_t decoded_size = 0;
    uint8_t *track;
    const size_t current_start = 0x31a9u;
    const size_t historical_start = 0x3a90u - 0x400u;
    const size_t payload_size = 0x28000u;

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_decode ( source, source_size, &logical, &logical_size, NULL ) );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_profile_init_default ( &profile,
                                           MZ_QD_IMAGE_FORMAT_FLASHFLOPPY ) );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_encode ( logical, logical_size, &profile,
                             &encoded, &encoded_size ) );
    track = encoded + profile.track_offset;
    memmove ( track + historical_start, track + current_start, payload_size );
    memset ( track + current_start, profile.blank_filler,
             historical_start - current_start );

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_decode ( encoded, encoded_size, &decoded,
                             &decoded_size, NULL ) );
    TEST_ASSERT_EQUAL_size_t ( logical_size, decoded_size );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( logical, decoded, logical_size );
    free ( decoded );
    free ( encoded );
    free ( logical );
    free ( source );
}

static void test_legacy_encode_is_canonical_and_readable ( void ) {
    size_t source_size;
    uint8_t *source = make_one_file_hxc ( &source_size );
    uint8_t *logical = NULL;
    size_t logical_size = 0;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    uint8_t *decoded = NULL;
    size_t decoded_size = 0;
    mz_qd_image_profile_t profile;
    memset ( &profile, 0, sizeof ( profile ) );
    profile.format = MZ_QD_IMAGE_FORMAT_SHARP_LOGICAL;

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_decode ( source, source_size, &logical, &logical_size, NULL ) );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_encode ( logical, logical_size, &profile,
                             &encoded, &encoded_size ) );
    TEST_ASSERT_EQUAL_size_t ( 0xf00f, encoded_size );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( logical, encoded, logical_size );
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_decode ( encoded, encoded_size, &decoded, &decoded_size, NULL ) );
    TEST_ASSERT_EQUAL_size_t ( encoded_size, decoded_size );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( encoded, decoded, encoded_size );
    free ( decoded );
    free ( encoded );
    free ( logical );
    free ( source );
}

static void test_legacy_encode_honors_requested_work_size ( void ) {
    const uint8_t logical[8] = { 0x00, 0x16, 0x16, 0xa5, 0x00, 'C', 'R', 'C' };
    mz_qd_image_profile_t profile;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    memset ( &profile, 0, sizeof ( profile ) );
    profile.format = MZ_QD_IMAGE_FORMAT_SHARP_LOGICAL;
    profile.container_size = 82958u;

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_encode ( logical, sizeof ( logical ), &profile,
                             &encoded, &encoded_size ) );
    TEST_ASSERT_EQUAL_size_t ( profile.container_size, encoded_size );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( logical, encoded, sizeof ( logical ) );
    TEST_ASSERT_EQUAL_UINT8 ( 0x00, encoded[encoded_size - 1u] );
    free ( encoded );
}

static void test_physical_encode_rejects_capacity_overflow ( void ) {
    const uint8_t logical[8] = { 0x00, 0x16, 0x16, 0xa5, 0x00, 'C', 'R', 'C' };
    mz_qd_image_profile_t profile = make_writable_profile ( MZ_QD_IMAGE_FORMAT_HXC );
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    profile.window_end = profile.window_start + 16u;
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_ERROR_CAPACITY,
        mz_qd_image_encode ( logical, sizeof ( logical ), &profile,
                             &encoded, &encoded_size ) );
    TEST_ASSERT_NULL ( encoded );
    TEST_ASSERT_EQUAL_size_t ( 0, encoded_size );

    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
        mz_qd_image_profile_init_default ( &profile,
                                           MZ_QD_IMAGE_FORMAT_FLASHFLOPPY ) );
    profile.window_end = profile.window_start + 0x28000u - 1u;
    TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_ERROR_CAPACITY,
        mz_qd_image_encode ( logical, sizeof ( logical ), &profile,
                             &encoded, &encoded_size ) );
    TEST_ASSERT_NULL ( encoded );
    TEST_ASSERT_EQUAL_size_t ( 0, encoded_size );
}

static void test_default_profiles_create_readable_blank_images ( void ) {
    const uint8_t logical[8] = { 0x00, 0x16, 0x16, 0xa5, 0x00, 'C', 'R', 'C' };
    const mz_qd_image_format_t formats[3] = {
        MZ_QD_IMAGE_FORMAT_SHARP_LOGICAL,
        MZ_QD_IMAGE_FORMAT_HXC,
        MZ_QD_IMAGE_FORMAT_FLASHFLOPPY
    };
    unsigned i;

    for ( i = 0; i < 3u; ++i ) {
        mz_qd_image_profile_t profile;
        mz_qd_image_format_t detected = MZ_QD_IMAGE_FORMAT_UNKNOWN;
        uint8_t *encoded = NULL;
        size_t encoded_size = 0;
        uint8_t *decoded = NULL;
        size_t decoded_size = 0;
        TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
            mz_qd_image_profile_init_default ( &profile, formats[i] ) );
        TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
            mz_qd_image_encode ( logical, sizeof ( logical ), &profile,
                                 &encoded, &encoded_size ) );
        TEST_ASSERT_EQUAL_size_t ( profile.container_size, encoded_size );
        TEST_ASSERT_EQUAL_INT ( MZ_QD_IMAGE_OK,
            mz_qd_image_decode ( encoded, encoded_size, &decoded,
                                 &decoded_size, &detected ) );
        TEST_ASSERT_EQUAL_INT ( formats[i], detected );
        TEST_ASSERT_EQUAL_UINT8 ( 0u, decoded[4] );
        free ( decoded );
        free ( encoded );
    }
}

int main ( void ) {
    UNITY_BEGIN ();
    RUN_TEST ( test_detects_supported_variants );
    RUN_TEST ( test_logical_image_is_validated_and_copied );
    RUN_TEST ( test_odd_logical_block_count_is_rejected );
    RUN_TEST ( test_blank_physical_variants_become_empty_logical_image );
    RUN_TEST ( test_hxc_mfm_frames_decode_to_mzq_stream );
    RUN_TEST ( test_bad_physical_frame_crc_is_rejected );
    RUN_TEST ( test_unsupported_hxc_geometry_is_rejected );
    RUN_TEST ( test_invalid_descriptor_is_rejected );
    RUN_TEST ( test_physical_encode_round_trip );
    RUN_TEST ( test_flashfloppy_encode_uses_real_mz_timing_placement );
    RUN_TEST ( test_hxc_encode_matches_mario_reference_layout );
    RUN_TEST ( test_two_file_physical_round_trip );
    RUN_TEST ( test_reader_accepts_historical_qdf2qd_placement );
    RUN_TEST ( test_legacy_encode_is_canonical_and_readable );
    RUN_TEST ( test_legacy_encode_honors_requested_work_size );
    RUN_TEST ( test_physical_encode_rejects_capacity_overflow );
    RUN_TEST ( test_default_profiles_create_readable_blank_images );
    return UNITY_END ();
}
