/*
 * Copyright (C) 2015-2017  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of DualBootPatcher
 *
 * DualBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DualBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DualBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mbbootimg/format/loki_reader_p.h"

#include <algorithm>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "mbcommon/endian.h"
#include "mbcommon/file.h"
#include "mbcommon/file_util.h"
#include "mbcommon/optional.h"
#include "mbcommon/string.h"

#include "mbbootimg/entry.h"
#include "mbbootimg/format/align_p.h"
#include "mbbootimg/format/android_error.h"
#include "mbbootimg/format/android_reader_p.h"
#include "mbbootimg/format/loki_error.h"
#include "mbbootimg/header.h"
#include "mbbootimg/reader.h"
#include "mbbootimg/reader_p.h"


namespace mb
{
namespace bootimg
{
namespace loki
{

LokiFormatReader::LokiFormatReader(Reader &reader)
    : FormatReader(reader)
    , _hdr()
    , _loki_hdr()
{
}

LokiFormatReader::~LokiFormatReader() = default;

int LokiFormatReader::type()
{
    return FORMAT_LOKI;
}

std::string LokiFormatReader::name()
{
    return FORMAT_NAME_LOKI;
}

/*!
 * \brief Perform a bid
 *
 * \return
 *   * If \>= 0, the number of bits that conform to the Loki format
 *   * -2 if this is a bid that can't be won
 *   * -1 if an error occurs
 */
int LokiFormatReader::bid(File &file, int best_bid)
{
    int bid = 0;

    if (best_bid >= static_cast<int>(
            android::BOOT_MAGIC_SIZE + LOKI_MAGIC_SIZE) * 8) {
        // This is a bid we can't win, so bail out
        return -2;
    }

    // Find the Loki header
    uint64_t loki_offset;
    if (find_loki_header(_reader, file, _loki_hdr, loki_offset)) {
        // Update bid to account for matched bits
        _loki_offset = loki_offset;
        bid += static_cast<int>(LOKI_MAGIC_SIZE * 8);
    } else if (_reader.error().category() == loki_error_category()) {
        // Header not found. This can't be a Loki boot image.
        return 0;
    } else {
        return -1;
    }

    // Find the Android header
    uint64_t header_offset;
    if (android::AndroidFormatReader::find_header(
            _reader, file, LOKI_MAX_HEADER_OFFSET, _hdr, header_offset)) {
        // Update bid to account for matched bits
        _header_offset = header_offset;
        bid += static_cast<int>(android::BOOT_MAGIC_SIZE * 8);
    } else if (_reader.error() == android::AndroidError::HeaderNotFound
            || _reader.error() == android::AndroidError::HeaderOutOfBounds) {
        // Header not found. This can't be an Android boot image.
        return 0;
    } else {
        return -1;
    }

    return bid;
}

bool LokiFormatReader::read_header(File &file, Header &header)
{
    uint64_t kernel_offset;
    uint64_t ramdisk_offset;
    uint64_t dt_offset = 0;
    uint32_t kernel_size;
    uint32_t ramdisk_size;

    // A bid might not have been performed if the user forced a particular
    // format
    if (!_loki_offset) {
        uint64_t loki_offset;
        if (!find_loki_header(_reader, file, _loki_hdr, loki_offset)) {
            return false;
        }
        _loki_offset = loki_offset;
    }
    if (!_header_offset) {
        uint64_t header_offset;
        if (!android::AndroidFormatReader::find_header(
                _reader, file, android::MAX_HEADER_OFFSET, _hdr,
                header_offset)) {
            return false;
        }
        _header_offset = header_offset;
    }

    // New-style images record the original values of changed fields in the
    // Android header
    bool ret;
    if (_loki_hdr.orig_kernel_size != 0
            && _loki_hdr.orig_ramdisk_size != 0
            && _loki_hdr.ramdisk_addr != 0) {
        ret =  read_header_new(_reader, file, _hdr, _loki_hdr, header,
                               kernel_offset, kernel_size,
                               ramdisk_offset, ramdisk_size,
                               dt_offset);
    } else {
        ret =  read_header_old(_reader, file, _hdr, _loki_hdr, header,
                               kernel_offset, kernel_size,
                               ramdisk_offset, ramdisk_size);
    }
    if (!ret) {
        return ret;
    }

    std::vector<SegmentReaderEntry> entries;

    entries.push_back({
        ENTRY_TYPE_KERNEL, kernel_offset, kernel_size, false
    });
    entries.push_back({
        ENTRY_TYPE_RAMDISK, ramdisk_offset, ramdisk_size, false
    });
    if (_hdr.dt_size > 0 && dt_offset != 0) {
        entries.push_back({
            ENTRY_TYPE_DEVICE_TREE, dt_offset, _hdr.dt_size, false
        });
    }

    return _seg.set_entries(_reader, std::move(entries));
}

bool LokiFormatReader::read_entry(File &file, Entry &entry)
{
    return _seg.read_entry(file, entry, _reader);
}

bool LokiFormatReader::go_to_entry(File &file, Entry &entry, int entry_type)
{
    return _seg.go_to_entry(file, entry, entry_type, _reader);
}

bool LokiFormatReader::read_data(File &file, void *buf, size_t buf_size,
                                 size_t &bytes_read)
{
    return _seg.read_data(file, buf, buf_size, bytes_read, _reader);
}

/*!
 * \brief Find and read Loki boot image header
 *
 * \note The integral fields in the header will be converted to the host's byte
 *       order.
 *
 * \pre The file position can be at any offset prior to calling this function.
 *
 * \post The file pointer position is undefined after this function returns.
 *       Use File::seek() to return to a known position.
 *
 * \param[in] reader Reader
 * \param[in] file File handle
 * \param[out] header_out Pointer to store header
 * \param[out] offset_out Pointer to store header offset
 *
 * \return
 *   * True if the header is found
 *   * False with a LokiError if the header is not found
 *   * False if any file operation fails
 */
bool LokiFormatReader::find_loki_header(Reader &reader, File &file,
                                        LokiHeader &header_out,
                                        uint64_t &offset_out)
{
    LokiHeader header;

    auto seek_ret = file.seek(LOKI_MAGIC_OFFSET, SEEK_SET);
    if (!seek_ret) {
        reader.set_error(seek_ret.error(),
                         "Loki magic not found: %s",
                         seek_ret.error().message().c_str());
        if (file.is_fatal()) { reader.set_fatal(); }
        return false;
    }

    auto n = file_read_fully(file, &header, sizeof(header));
    if (!n) {
        reader.set_error(n.error(),
                         "Failed to read header: %s",
                         n.error().message().c_str());
        if (file.is_fatal()) { reader.set_fatal(); }
        return false;
    } else if (n.value() != sizeof(header)) {
        reader.set_error(LokiError::LokiHeaderTooSmall,
                         "Too small to be Loki image");
        return false;
    }

    if (memcmp(header.magic, LOKI_MAGIC, LOKI_MAGIC_SIZE) != 0) {
        reader.set_error(LokiError::InvalidLokiMagic);
        return false;
    }

    loki_fix_header_byte_order(header);
    header_out = header;
    offset_out = LOKI_MAGIC_OFFSET;

    return true;
}

/*!
 * \brief Find and read Loki ramdisk address
 *
 * \pre The file position can be at any offset prior to calling this function.
 *
 * \post The file pointer position is undefined after this function returns.
 *       Use File::seek() to return to a known position.
 *
 * \param[in] reader Reader to set error message
 * \param[in] file File handle
 * \param[in] hdr Android header
 * \param[in] loki_hdr Loki header
 * \param[out] ramdisk_addr_out Pointer to store ramdisk address
 *
 * \return
 *   * True if the ramdisk address is found
 *   * False with a LokiError if the ramdisk address is not found
 *   * False if any file operation fails
 */
bool LokiFormatReader::find_ramdisk_address(Reader &reader, File &file,
                                            const android::AndroidHeader &hdr,
                                            const LokiHeader &loki_hdr,
                                            uint32_t &ramdisk_addr_out)
{
    // If the boot image was patched with a newer version of loki, find the
    // ramdisk offset in the shell code
    uint32_t ramdisk_addr = 0;

    if (loki_hdr.ramdisk_addr != 0) {
        uint64_t offset = 0;

        auto result_cb = [](File &file_, void *userdata, uint64_t offset_)
                -> oc::result<FileSearchAction> {
            (void) file_;
            auto offset_ptr = static_cast<uint64_t *>(userdata);
            *offset_ptr = offset_;
            return FileSearchAction::Continue;
        };

        auto ret = file_search(file, -1, -1, 0, LOKI_SHELLCODE,
                               LOKI_SHELLCODE_SIZE - 9, 1, result_cb, &offset);
        if (!ret) {
            reader.set_error(ret.error(),
                             "Failed to search for Loki shellcode: %s",
                             ret.error().message().c_str());
            if (file.is_fatal()) { reader.set_fatal(); }
            return false;
        }

        if (offset == 0) {
            reader.set_error(LokiError::ShellcodeNotFound);
            return false;
        }

        offset += LOKI_SHELLCODE_SIZE - 5;

        auto seek_ret = file.seek(static_cast<int64_t>(offset), SEEK_SET);
        if (!seek_ret) {
            reader.set_error(seek_ret.error(),
                             "Failed to seek to ramdisk address offset: %s",
                             seek_ret.error().message().c_str());
            if (file.is_fatal()) { reader.set_fatal(); }
            return false;
        }

        auto n = file_read_fully(file, &ramdisk_addr, sizeof(ramdisk_addr));
        if (!n) {
            reader.set_error(n.error(),
                             "Failed to read ramdisk address offset: %s",
                             n.error().message().c_str());
            if (file.is_fatal()) { reader.set_fatal(); }
            return false;
        } else if (n.value() != sizeof(ramdisk_addr)) {
            reader.set_error(LokiError::UnexpectedEndOfFile,
                             "Unexpected EOF when reading ramdisk address");
            return false;
        }

        ramdisk_addr = mb_le32toh(ramdisk_addr);
    } else {
        // Otherwise, use the default for jflte (- 0x00008000 + 0x02000000)

        if (hdr.kernel_addr > UINT32_MAX - 0x01ff8000) {
            reader.set_error(LokiError::InvalidKernelAddress,
                             "Invalid kernel address: %" PRIu32,
                             hdr.kernel_addr);
            return false;
        }

        ramdisk_addr = hdr.kernel_addr + 0x01ff8000;
    }

    ramdisk_addr_out = ramdisk_addr;
    return true;
}

/*!
 * \brief Find gzip ramdisk offset in old-style Loki image
 *
 * This function will search for gzip headers (`0x1f8b08`) with a flags byte of
 * `0x00` or `0x08`. It will find the first occurrence of either magic string.
 * If both are found, the one with the flags byte set to `0x08` takes precedence
 * as it indiciates that the original filename field is set. This is usually the
 * case for ramdisks packed via the `gzip` command line tool.
 *
 * \pre The file position can be at any offset prior to calling this function.
 *
 * \post The file pointer position is undefined after this function returns.
 *       Use File::seek() to return to a known position.
 *
 * \param[in] reader Reader to set error message
 * \param[in] file File handle
 * \param[in] start_offset Starting offset for search
 * \param[out] gzip_offset_out Pointer to store gzip ramdisk offset
 *
 * \return
 *   * True if a gzip offset is found
 *   * False with a LokiError if no gzip offsets are found
 *   * False if any file operation fails
 */
bool LokiFormatReader::find_gzip_offset_old(Reader &reader, File &file,
                                            uint32_t start_offset,
                                            uint64_t &gzip_offset_out)
{
    struct SearchResult
    {
        optional<uint64_t> flag0_offset;
        optional<uint64_t> flag8_offset;
    };

    // gzip header:
    // byte 0-1 : magic bytes 0x1f, 0x8b
    // byte 2   : compression (0x08 = deflate)
    // byte 3   : flags
    // byte 4-7 : modification timestamp
    // byte 8   : compression flags
    // byte 9   : operating system

    static const unsigned char gzip_deflate_magic[] = { 0x1f, 0x8b, 0x08 };

    SearchResult result = {};

    // Find first result with flags == 0x00 and flags == 0x08
    auto result_cb = [](File &file_, void *userdata, uint64_t offset)
            -> oc::result<FileSearchAction> {
        auto result_ = static_cast<SearchResult *>(userdata);
        unsigned char flags;

        // Stop early if possible
        if (result_->flag0_offset && result_->flag8_offset) {
            return FileSearchAction::Stop;
        }

        // Save original position
        OUTCOME_TRY(orig_offset, file_.seek(0, SEEK_CUR));

        // Seek to flags byte
        OUTCOME_TRYV(file_.seek(static_cast<int64_t>(offset + 3), SEEK_SET));

        // Read next bytes for flags
        auto n = file_read_fully(file_, &flags, sizeof(flags));
        if (!n) {
            return n.as_failure();
        } else if (n.value() != sizeof(flags)) {
            // EOF
            return FileSearchAction::Stop;
        }

        if (!result_->flag0_offset && flags == 0x00) {
            result_->flag0_offset = offset;
        } else if (!result_->flag8_offset && flags == 0x08) {
            result_->flag8_offset = offset;
        }

        // Restore original position as per contract
        OUTCOME_TRYV(file_.seek(static_cast<int64_t>(orig_offset), SEEK_SET));

        return FileSearchAction::Continue;
    };

    auto ret = file_search(file, start_offset, -1, 0, gzip_deflate_magic,
                           sizeof(gzip_deflate_magic), -1, result_cb, &result);
    if (!ret) {
        reader.set_error(ret.error(),
                         "Failed to search for gzip magic: %s",
                         ret.error().message().c_str());
        if (file.is_fatal()) { reader.set_fatal(); }
        return false;
    }

    // Prefer gzip header with original filename flag since most loki'd boot
    // images will have been compressed manually with the gzip tool
    if (result.flag8_offset) {
        gzip_offset_out = *result.flag8_offset;
    } else if (result.flag0_offset) {
        gzip_offset_out = *result.flag0_offset;
    } else {
        reader.set_error(LokiError::NoRamdiskGzipHeaderFound);
        return false;
    }

    return true;
}

/*!
 * \brief Find ramdisk size in old-style Loki image
 *
 * \pre The file position can be at any offset prior to calling this function.
 *
 * \post The file pointer position is undefined after this function returns.
 *       Use File::seek() to return to a known position.
 *
 * \param[in] reader Reader to set error message
 * \param[in] file File handle
 * \param[in] hdr Android header
 * \param[in] ramdisk_offset Offset of ramdisk in image
 * \param[out] ramdisk_size_out Pointer to store ramdisk size
 *
 * \return
 *   * True if the ramdisk size is found
 *   * False with a LokiError if the ramdisk size is not found
 *   * False if any file operation fails
 */
bool LokiFormatReader::find_ramdisk_size_old(Reader &reader, File &file,
                                             const android::AndroidHeader &hdr,
                                             uint32_t ramdisk_offset,
                                             uint32_t &ramdisk_size_out)
{
    int32_t aboot_size;

    // If the boot image was patched with an old version of loki, the ramdisk
    // size is not stored properly. We'll need to guess the size of the archive.

    // The ramdisk is supposed to be from the gzip header to EOF, but loki needs
    // to store a copy of aboot, so it is put in the last 0x200 bytes of the
    // file.
    if (is_lg_ramdisk_address(hdr.ramdisk_addr)) {
        aboot_size = static_cast<int32_t>(hdr.page_size);
    } else {
        aboot_size = 0x200;
    }

    auto aboot_offset = file.seek(-aboot_size, SEEK_END);
    if (!aboot_offset) {
        reader.set_error(aboot_offset.error(),
                         "Failed to seek to end of file: %s",
                         aboot_offset.error().message().c_str());
        if (file.is_fatal()) { reader.set_fatal(); }
        return false;
    }

    if (ramdisk_offset > aboot_offset.value()) {
        reader.set_error(LokiError::RamdiskOffsetGreaterThanAbootOffset);
        return false;
    }

    // Ignore zero padding as we might strip away too much
#if 1
    ramdisk_size_out = static_cast<uint32_t>(
            aboot_offset.value() - ramdisk_offset);
    return true;
#else
    char buf[1024];

    // Search backwards to find non-zero byte
    uint64_t cur_offset = aboot_offset.value();

    while (cur_offset > ramdisk_offset) {
        size_t to_read = std::min<uint64_t>(
                sizeof(buf), cur_offset - ramdisk_offset);
        cur_offset -= to_read;

        auto seek_ret = file.seek(cur_offset, SEEK_SET);
        if (!seek_ret) {
            reader.set_error(seek_ret.error(),
                             "Failed to seek: %s",
                             seek_ret.error().message().c_str());
            if (file.is_fatal()) { reader.set_fatal(); }
            return false;
        }

        auto n = file_read_fully(file, buf, to_read);
        if (!n) {
            reader.set_error(n.error(),
                             "Failed to read: %s",
                             n.error().message().c_str());
            if (file.is_fatal()) { reader.set_fatal(); }
            return false;
        } else if (n.value() != to_read) {
            reader.set_error(LokiError::UnexpectedFileTruncation);
            return false;
        }

        for (size_t i = n.value(); i-- > 0; ) {
            if (buf[i] != '\0') {
                ramdisk_size_out = cur_offset - ramdisk_offset + i;
                return true;
            }
        }
    }

    reader.set_error(LokiError::FailedToDetermineRamdiskSize);
    return false;
#endif
}

/*!
 * \brief Find size of Linux kernel in boot image
 *
 * \pre The file position can be at any offset prior to calling this function.
 *
 * \post The file pointer position is undefined after this function returns.
 *       Use File::seek() to return to a known position.
 *
 * \param[in] reader Reader to set error message
 * \param[in] file File handle
 * \param[in] kernel_offset Offset of kernel in boot image
 * \param[out] kernel_size_out Pointer to store kernel size
 *
 * \return
 *   * True if the kernel size is found
 *   * False with the error set to a LokiError if the kernel size cannot be
 *     found
 *   * False if any file operation fails
 */
bool LokiFormatReader::find_linux_kernel_size(Reader &reader, File &file,
                                              uint32_t kernel_offset,
                                              uint32_t &kernel_size_out)
{
    uint32_t kernel_size;

    // If the boot image was patched with an early version of loki, the
    // original kernel size is not stored in the loki header properly (or in the
    // shellcode). The size is stored in the kernel image's header though, so
    // we'll use that.
    // http://www.simtec.co.uk/products/SWLINUX/files/booting_article.html#d0e309
    auto seek_ret = file.seek(kernel_offset + 0x2c, SEEK_SET);
    if (!seek_ret) {
        reader.set_error(seek_ret.error(),
                         "Failed to seek to kernel header: %s",
                         seek_ret.error().message().c_str());
        if (file.is_fatal()) { reader.set_fatal(); }
        return false;
    }

    auto n = file_read_fully(file, &kernel_size, sizeof(kernel_size));
    if (!n) {
        reader.set_error(n.error(),
                         "Failed to read size from kernel header: %s",
                         n.error().message().c_str());
        if (file.is_fatal()) { reader.set_fatal(); }
        return false;
    } else if (n.value() != sizeof(kernel_size)) {
        reader.set_error(LokiError::UnexpectedEndOfFile,
                         "Unexpected EOF when reading kernel header");
        return false;
    }

    kernel_size_out = mb_le32toh(kernel_size);
    return true;
}

/*!
 * \brief Read header for old-style Loki image
 *
 * \param[in] reader Reader to set error message
 * \param[in] file File handle
 * \param[in] hdr Android header for image
 * \param[in] loki_hdr Loki header for image
 * \param[out] header Header instance to store header values
 * \param[out] kernel_offset_out Pointer to store kernel offset
 * \param[out] kernel_size_out Pointer to store kernel size
 * \param[out] ramdisk_offset_out Pointer to store ramdisk offset
 * \param[out] ramdisk_size_out Pointer to store ramdisk size
 *
 * \return Whether the header is successfully read
 */
bool LokiFormatReader::read_header_old(Reader &reader, File &file,
                                       const android::AndroidHeader &hdr,
                                       const LokiHeader &loki_hdr,
                                       Header &header,
                                       uint64_t &kernel_offset_out,
                                       uint32_t &kernel_size_out,
                                       uint64_t &ramdisk_offset_out,
                                       uint32_t &ramdisk_size_out)
{
    uint32_t tags_addr;
    uint32_t kernel_size;
    uint32_t ramdisk_size;
    uint32_t ramdisk_addr;
    uint64_t gzip_offset;

    if (hdr.page_size == 0) {
        reader.set_error(LokiError::PageSizeCannotBeZero);
        return false;
    }

    // The kernel tags address is invalid in the old loki images, so use the
    // default for jflte
    tags_addr = hdr.kernel_addr - android::DEFAULT_KERNEL_OFFSET
            + android::DEFAULT_TAGS_OFFSET;

    // Try to guess kernel size
    if (!find_linux_kernel_size(reader, file, hdr.page_size, kernel_size)) {
        return false;
    }

    // Look for gzip offset for the ramdisk
    if (!find_gzip_offset_old(
            reader, file, hdr.page_size + kernel_size
                    + align_page_size<uint32_t>(kernel_size, hdr.page_size),
            gzip_offset)) {
        return false;
    }

    // Try to guess ramdisk size
    if (!find_ramdisk_size_old(reader, file, hdr,
                               static_cast<uint32_t>(gzip_offset),
                               ramdisk_size)) {
        return false;
    }

    // Guess original ramdisk address
    if (!find_ramdisk_address(reader, file, hdr, loki_hdr, ramdisk_addr)) {
        return false;
    }

    kernel_size_out = kernel_size;
    ramdisk_size_out = ramdisk_size;

    char board_name[sizeof(hdr.name) + 1];
    char cmdline[sizeof(hdr.cmdline) + 1];

    strncpy(board_name, reinterpret_cast<const char *>(hdr.name),
            sizeof(hdr.name));
    strncpy(cmdline, reinterpret_cast<const char *>(hdr.cmdline),
            sizeof(hdr.cmdline));
    board_name[sizeof(hdr.name)] = '\0';
    cmdline[sizeof(hdr.cmdline)] = '\0';

    header.set_supported_fields(OLD_SUPPORTED_FIELDS);
    header.set_board_name({board_name});
    header.set_kernel_cmdline({cmdline});
    header.set_page_size(hdr.page_size);
    header.set_kernel_address(hdr.kernel_addr);
    header.set_ramdisk_address(ramdisk_addr);
    header.set_secondboot_address(hdr.second_addr);
    header.set_kernel_tags_address(tags_addr);

    uint64_t pos = 0;

    // pos cannot overflow due to the nature of the operands (adding UINT32_MAX
    // a few times can't overflow a uint64_t). File length overflow is checked
    // during read.

    // Header
    pos += hdr.page_size;

    // Kernel
    kernel_offset_out = pos;
    pos += kernel_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    // Ramdisk
    ramdisk_offset_out = pos = gzip_offset;
    pos += ramdisk_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    return true;
}

/*!
 * \brief Read header for new-style Loki image
 *
 * \param[in] reader Reader to set error message
 * \param[in] file File handle
 * \param[in] hdr Android header for image
 * \param[in] loki_hdr Loki header for image
 * \param[out] header Header instance to store header values
 * \param[out] kernel_offset_out Pointer to store kernel offset
 * \param[out] kernel_size_out Pointer to store kernel size
 * \param[out] ramdisk_offset_out Pointer to store ramdisk offset
 * \param[out] ramdisk_size_out Pointer to store ramdisk size
 * \param[out] dt_offset_out Pointer to store device tree offset
 *
 * \return Whether the header is successfully read
 */
bool LokiFormatReader::read_header_new(Reader &reader, File &file,
                                       const android::AndroidHeader &hdr,
                                       const LokiHeader &loki_hdr,
                                       Header &header,
                                       uint64_t &kernel_offset_out,
                                       uint32_t &kernel_size_out,
                                       uint64_t &ramdisk_offset_out,
                                       uint32_t &ramdisk_size_out,
                                       uint64_t &dt_offset_out)
{
    uint32_t fake_size;
    uint32_t ramdisk_addr;

    if (hdr.page_size == 0) {
        reader.set_error(LokiError::PageSizeCannotBeZero);
        return false;
    }

    if (is_lg_ramdisk_address(hdr.ramdisk_addr)) {
        fake_size = hdr.page_size;
    } else {
        fake_size = 0x200;
    }

    // Find original ramdisk address
    if (!find_ramdisk_address(reader, file, hdr, loki_hdr, ramdisk_addr)) {
        return false;
    }

    // Restore original values in boot image header
    kernel_size_out = loki_hdr.orig_kernel_size;
    ramdisk_size_out = loki_hdr.orig_ramdisk_size;

    char board_name[sizeof(hdr.name) + 1];
    char cmdline[sizeof(hdr.cmdline) + 1];

    strncpy(board_name, reinterpret_cast<const char *>(hdr.name),
            sizeof(hdr.name));
    strncpy(cmdline, reinterpret_cast<const char *>(hdr.cmdline),
            sizeof(hdr.cmdline));
    board_name[sizeof(hdr.name)] = '\0';
    cmdline[sizeof(hdr.cmdline)] = '\0';

    header.set_supported_fields(NEW_SUPPORTED_FIELDS);
    header.set_board_name({board_name});
    header.set_kernel_cmdline({cmdline});
    header.set_page_size(hdr.page_size);
    header.set_kernel_address(hdr.kernel_addr);
    header.set_ramdisk_address(ramdisk_addr);
    header.set_secondboot_address(hdr.second_addr);
    header.set_kernel_tags_address(hdr.tags_addr);

    uint64_t pos = 0;

    // pos cannot overflow due to the nature of the operands (adding UINT32_MAX
    // a few times can't overflow a uint64_t). File length overflow is checked
    // during read.

    // Header
    pos += hdr.page_size;

    // Kernel
    kernel_offset_out = pos;
    pos += loki_hdr.orig_kernel_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    // Ramdisk
    ramdisk_offset_out = pos;
    pos += loki_hdr.orig_ramdisk_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    // Device tree
    if (hdr.dt_size != 0) {
        pos += fake_size;
    }
    dt_offset_out = pos;
    pos += hdr.dt_size;
    pos += align_page_size<uint64_t>(pos, hdr.page_size);

    return true;
}

}

/*!
 * \brief Enable support for Loki boot image format
 *
 * \return Whether the format is successfully enabled
 */
bool Reader::enable_format_loki()
{
    return register_format(std::make_unique<loki::LokiFormatReader>(*this));
}

}
}
