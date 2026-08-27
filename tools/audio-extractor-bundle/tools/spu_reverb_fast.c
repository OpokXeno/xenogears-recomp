#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#define WORK_SIZE ((0x10000 - 0xF204) * 4)

static const uint16_t r[] = {
    0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680,
    0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
    0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2,
    0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000,
};

static const int32_t resample_coefficients[] = {
    -0x0001, 0x0002,  -0x000A, 0x0023,  -0x0067, 0x010A,  -0x0268,
    0x0534,  -0x0B90, 0x2806, 0x2806,  -0x0B90, 0x0534,  -0x0268,
    0x010A,  -0x0067, 0x0023,  -0x000A, 0x0002,  -0x0001,
};

static int32_t clamp16(int64_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int32_t)value;
}

static int32_t multiply15(uint16_t volume, int64_t sample) {
    return (int32_t)(((int64_t)(int16_t)volume * sample) >> 15);
}

static int32_t negate16(uint16_t value) {
    int32_t signed_value = (int16_t)value;
    return signed_value == -32768 ? 32767 : -signed_value;
}

static int64_t iir_complement(int16_t sample, uint16_t alpha_raw) {
    int32_t alpha = (int16_t)alpha_raw;
    if (alpha == -32768)
        return sample == -32768 ? 0 : (int64_t)sample * -65536;
    return (int64_t)sample * (32768 - alpha);
}

static size_t work_index(size_t cursor, uint16_t reg, int32_t bias) {
    int64_t index = (int64_t)cursor + (int64_t)reg * 4 + bias;
    index %= WORK_SIZE;
    if (index < 0) index += WORK_SIZE;
    return (size_t)index;
}

static int16_t read_work(const int16_t *work, size_t cursor, uint16_t reg,
                         int32_t bias) {
    return work[work_index(cursor, reg, bias)];
}

static void write_work(int16_t *work, size_t cursor, uint16_t reg,
                       int64_t value) {
    work[work_index(cursor, reg, 0)] = (int16_t)clamp16(value);
}

size_t xg_spu_reverb_mode4(
    int32_t *mix, size_t source_frames, size_t capacity_frames,
    const int32_t *send, size_t send_length, int32_t depth,
    const uint32_t *change_frames, const int32_t *change_depths,
    size_t change_count, size_t tail_frames) {
    int16_t *work = calloc(WORK_SIZE, sizeof(*work));
    if (work == NULL) return SIZE_MAX;

    int16_t downsample[2][64] = {{0}};
    int16_t upsample[2][32] = {{0}};
    const uint16_t dapf1 = r[0], dapf2 = r[1], viir = r[2];
    const uint16_t vcomb[4] = {r[3], r[4], r[5], r[6]};
    const uint16_t vwall = r[7], vapf1 = r[8], vapf2 = r[9];
    const uint16_t same_dest[2] = {r[10], r[11]};
    const uint16_t comb_source[2][4] = {
        {r[12], r[14], r[20], r[22]},
        {r[13], r[15], r[21], r[23]},
    };
    const uint16_t same_source[2] = {r[16], r[17]};
    const uint16_t diff_dest[2] = {r[18], r[19]};
    const uint16_t diff_source[2] = {r[25], r[24]};
    const uint16_t mix_dest_a[2] = {r[26], r[27]};
    const uint16_t mix_dest_b[2] = {r[28], r[29]};
    const uint16_t input_coefficient[2] = {r[30], r[31]};
    const size_t max_frames = source_frames + tail_frames;
    size_t cursor = 0, resample_position = 0, change_index = 0;
    int64_t last_wet_frame = -1;

    if (capacity_frames < max_frames) {
        free(work);
        return SIZE_MAX;
    }

    for (size_t frame = 0; frame < max_frames; ++frame) {
        while (change_index < change_count &&
               change_frames[change_index] <= frame)
            depth = change_depths[change_index++];

        for (size_t channel = 0; channel < 2; ++channel) {
            size_t send_index = frame * 2 + channel;
            downsample[channel][resample_position] = (int16_t)clamp16(
                send_index < send_length ? send[send_index] : 0);
        }

        if (resample_position & 1u) {
            int32_t downsampled[2];
            size_t start = (resample_position - 38) & 0x3Fu;
            for (size_t channel = 0; channel < 2; ++channel) {
                int64_t accumulator = 0;
                for (size_t index = 0; index < 20; ++index)
                    accumulator += (int64_t)resample_coefficients[index] *
                        downsample[channel][(start + index * 2) & 0x3Fu];
                accumulator += (int64_t)0x4000 *
                    downsample[channel][(start + 19) & 0x3Fu];
                downsampled[channel] = clamp16(accumulator >> 15);
            }

            for (size_t channel = 0; channel < 2; ++channel) {
                int32_t iir_input_same = clamp16((
                    (((int64_t)read_work(work, cursor, same_source[channel], 0) *
                      (int16_t)vwall) >> 14) +
                    (((int64_t)downsampled[channel] *
                      (int16_t)input_coefficient[channel]) >> 14)) >> 1);
                int32_t iir_input_diff = clamp16((
                    (((int64_t)read_work(work, cursor, diff_source[channel], 0) *
                      (int16_t)vwall) >> 14) +
                    (((int64_t)downsampled[channel] *
                      (int16_t)input_coefficient[channel]) >> 14)) >> 1);
                int16_t previous = read_work(work, cursor, same_dest[channel], -1);
                int32_t iir_same = clamp16((
                    (((int64_t)iir_input_same * (int16_t)viir) >> 14) +
                    (iir_complement(previous, viir) >> 14)) >> 1);
                previous = read_work(work, cursor, diff_dest[channel], -1);
                int32_t iir_diff = clamp16((
                    (((int64_t)iir_input_diff * (int16_t)viir) >> 14) +
                    (iir_complement(previous, viir) >> 14)) >> 1);
                write_work(work, cursor, same_dest[channel], iir_same);
                write_work(work, cursor, diff_dest[channel], iir_diff);

                int64_t accumulator = 0;
                for (size_t index = 0; index < 4; ++index)
                    accumulator +=
                        ((int64_t)read_work(work, cursor,
                                           comb_source[channel][index], 0) *
                         (int16_t)vcomb[index]) >> 14;
                int32_t feedback_a = read_work(
                    work, cursor, mix_dest_a[channel], -(int32_t)dapf1 * 4);
                int32_t feedback_b = read_work(
                    work, cursor, mix_dest_b[channel], -(int32_t)dapf2 * 4);
                int32_t mix_a = clamp16(
                    (accumulator +
                     (((int64_t)feedback_a * negate16(vapf1)) >> 14)) >> 1);
                int32_t mix_b = clamp16(
                    feedback_a +
                    (((((int64_t)mix_a * (int16_t)vapf1) >> 14) +
                      (((int64_t)feedback_b * negate16(vapf2)) >> 14)) >> 1));
                int32_t reverb_sample = clamp16(
                    feedback_b + (((int64_t)mix_b * (int16_t)vapf2) >> 15));
                upsample[channel][resample_position >> 1] =
                    (int16_t)reverb_sample;
                write_work(work, cursor, mix_dest_a[channel], mix_a);
                write_work(work, cursor, mix_dest_b[channel], mix_b);
            }
            cursor = cursor + 1 == WORK_SIZE ? 0 : cursor + 1;
        }

        int32_t wet[2];
        if (resample_position & 1u) {
            size_t start = ((resample_position >> 1) - 19) & 0x1Fu;
            for (size_t channel = 0; channel < 2; ++channel) {
                int64_t accumulator = 0;
                for (size_t index = 0; index < 20; ++index)
                    accumulator += (int64_t)resample_coefficients[index] *
                        upsample[channel][(start + index) & 0x1Fu];
                wet[channel] = clamp16(accumulator >> 14);
            }
        } else {
            size_t index = ((((resample_position >> 1) - 19) & 0x1Fu) + 9) & 0x1Fu;
            wet[0] = upsample[0][index];
            wet[1] = upsample[1][index];
        }

        uint16_t output_volume = (uint16_t)((uint32_t)depth << 8);
        int32_t wet_l = multiply15(output_volume, wet[0]);
        int32_t wet_r = multiply15(output_volume, wet[1]);
        mix[frame * 2] = clamp16((int64_t)mix[frame * 2] + wet_l);
        mix[frame * 2 + 1] = clamp16((int64_t)mix[frame * 2 + 1] + wet_r);
        if (llabs(wet_l) > 1 || llabs(wet_r) > 1)
            last_wet_frame = (int64_t)frame;
        resample_position = (resample_position + 1) & 0x3Fu;
    }

    free(work);
    size_t wet_frames = last_wet_frame < 0 ? 0 : (size_t)last_wet_frame + 1;
    return source_frames > wet_frames ? source_frames : wet_frames;
}
