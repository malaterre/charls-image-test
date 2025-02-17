// Copyright (c) Team CharLS.
// SPDX-License-Identifier: MIT

import portable_anymap_file;

import <charls/charls.h>;

import <chrono>;
import <filesystem>;
import <vector>;
import <fstream>;
import <cassert>;
import <format>;

using charls::interleave_mode;
using charls::jpegls_decoder;
using charls::jpegls_encoder;
using std::byte;
using std::format;
using std::ofstream;
using std::vector;
using std::chrono::steady_clock;
using std::filesystem::path;

namespace {

void puts(const std::string& str) noexcept
{
    std::puts(str.c_str());
}

void triplet_to_planar(vector<byte>& buffer, const size_t width, const size_t height, const int32_t bits_per_sample)
{
    vector<byte> work_buffer(buffer.size());

    const size_t samples_per_plane{width * height};

    if (bits_per_sample > 8)
    {
        const auto* source_buffer16{reinterpret_cast<const uint16_t*>(buffer.data())};
        auto* buffer16{reinterpret_cast<uint16_t*>(work_buffer.data())};
        for (size_t i{}; i != samples_per_plane; ++i)
        {
            buffer16[i] = source_buffer16[i * 3 + 0];
            buffer16[i + 1 * samples_per_plane] = source_buffer16[i * 3 + 1];
            buffer16[i + 2 * samples_per_plane] = source_buffer16[i * 3 + 2];
        }
    }
    else
    {
        for (size_t i{}; i != samples_per_plane; ++i)
        {
            work_buffer[i] = buffer[i * 3 + 0];
            work_buffer[i + 1 * samples_per_plane] = buffer[i * 3 + 1];
            work_buffer[i + 2 * samples_per_plane] = buffer[i * 3 + 2];
        }
    }

    swap(buffer, work_buffer);
}


[[nodiscard]] portable_anymap_file read_anymap_reference_file(const char* filename, const interleave_mode interleave_mode)
{
    portable_anymap_file reference_file(filename);

    if (interleave_mode == interleave_mode::none && reference_file.component_count() == 3)
    {
        triplet_to_planar(reference_file.image_data(), reference_file.width(), reference_file.height(), reference_file.bits_per_sample());
    }

    return reference_file;
}


[[nodiscard]] bool test_by_decoding(const vector<byte>& encoded_source, const vector<byte>& original_source, std::chrono::duration<double, std::milli>& decode_duration)
{
    jpegls_decoder decoder{encoded_source, true};

    vector<byte> decoded(decoder.destination_size());

    const auto start{steady_clock::now()};
    decoder.decode(decoded);
    decode_duration = steady_clock::now() - start;

    if (decoded.size() != original_source.size())
    {
        puts("Pixel data size doesn't match");
        return false;
    }

    if (decoder.near_lossless() == 0)
    {
        for (size_t i{}; i < original_source.size(); ++i)
        {
            if (decoded[i] != original_source[i])
            {
                puts("Pixel data value doesn't match");
                return false;
            }
        }
    }

    return true;
}

[[nodiscard]] const char* interleave_mode_to_string(const interleave_mode interleave_mode) noexcept
{
    switch (interleave_mode)
    {
    case interleave_mode::none:
        return "none";

    case interleave_mode::line:
        return "line";

    case interleave_mode::sample:
        return "sample";
    }

    assert(false);
    return "";
}

[[nodiscard]] path generate_output_filename(const path& source_filename, const interleave_mode interleave_mode)
{
    path output_filename{source_filename};
    output_filename.replace_filename(output_filename.stem().string() + "-" + interleave_mode_to_string(interleave_mode));
    output_filename.replace_extension(".jls");

    return output_filename;
}

[[nodiscard]] bool check_file(const path& source_filename, const interleave_mode interleave_mode = interleave_mode::none, bool color = false)
{
    const portable_anymap_file reference_file{read_anymap_reference_file(source_filename.string().c_str(), interleave_mode)};

    jpegls_encoder encoder;
    encoder.frame_info({reference_file.width(), reference_file.height(),
                        reference_file.bits_per_sample(), reference_file.component_count()})
        .interleave_mode(interleave_mode);

    vector<byte> charls_encoded_data(encoder.estimated_destination_size());
    encoder.destination(charls_encoded_data);

    const auto start{steady_clock::now()};
    const size_t encoded_size{encoder.encode(reference_file.image_data())};
    const auto encode_duration{steady_clock::now() - start};

    charls_encoded_data.resize(encoded_size);

    ofstream output(generate_output_filename(source_filename, interleave_mode).string().c_str(), ofstream::binary | ofstream::trunc);
    output.exceptions(ofstream::eofbit | ofstream::failbit | ofstream::badbit);
    output.write(reinterpret_cast<const char*>(charls_encoded_data.data()), static_cast<std::streamsize>(charls_encoded_data.size()));

    const double compression_ratio = static_cast<double>(reference_file.image_data().size()) / static_cast<double>(encoded_size);

    std::chrono::duration<double, std::milli> decode_duration;
    const bool result{test_by_decoding(charls_encoded_data, reference_file.image_data(), decode_duration)};

    const int interleave_mode_width{color ? 6 : 4};
    puts(format(" Info: original size = {}, encoded size = {}, interleave mode = {:{}}, compression ratio = {:.2}:1, encode time = {:.4} ms, decode time = {:.4} ms",
                   reference_file.image_data().size(), encoded_size, interleave_mode_to_string(interleave_mode), interleave_mode_width, compression_ratio,
                   std::chrono::duration<double, std::milli>(encode_duration).count(), decode_duration.count()));

    return result;
}

[[nodiscard]] bool check_color_file(const path& source_filename)
{
    auto result{check_file(source_filename, interleave_mode::none, true)};
    if (!result)
        return result;

    result = check_file(source_filename, interleave_mode::line, true);
    if (!result)
        return result;

    return check_file(source_filename, interleave_mode::sample, true);
}

} // namespace

int main(const int argc, const char* const argv[]) // NOLINT(bugprone-exception-escape)
try
{
    if (argc < 2)
    {
        puts("usage: charls_image_tester <directory-to-test>");
        return EXIT_FAILURE;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1]))
    {
        const bool monochrome_anymap{entry.path().extension() == ".pgm"};
        const bool color_anymap{entry.path().extension() == ".ppm"};

        if (monochrome_anymap || color_anymap)
        {
            puts(format("Checking file: {}", entry.path().string()));
            const bool result{monochrome_anymap ? check_file(entry.path()) : check_color_file(entry.path())};
            puts(format(" Status: {}", result ? "Passed" : "Failed"));
            if (!result)
                return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}
catch (const std::exception& error)
{
    puts(format("Unexpected failure: {}", error.what()));
    return EXIT_FAILURE;
}
