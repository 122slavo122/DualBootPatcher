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

#include "mbbootimg/format/android_writer_p.h"

#include <algorithm>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <openssl/sha.h>

#include "mbcommon/endian.h"
#include "mbcommon/file.h"
#include "mbcommon/file_util.h"
#include "mbcommon/string.h"

#include "mbbootimg/entry.h"
#include "mbbootimg/format/align_p.h"
#include "mbbootimg/format/android_error.h"
#include "mbbootimg/format/bump_defs.h"
#include "mbbootimg/header.h"
#include "mbbootimg/writer.h"
#include "mbbootimg/writer_p.h"


namespace mb
{
namespace bootimg
{
namespace android
{

AndroidFormatWriter::AndroidFormatWriter(Writer &writer, bool is_bump)
    : FormatWriter(writer)
    , _hdr()
    , _is_bump(is_bump)
    , _sha_ctx()
{
}

AndroidFormatWriter::~AndroidFormatWriter() = default;

int AndroidFormatWriter::type()
{
    if (_is_bump) {
        return FORMAT_BUMP;
    } else {
        return FORMAT_ANDROID;
    }
}

std::string AndroidFormatWriter::name()
{
    if (_is_bump) {
        return FORMAT_NAME_BUMP;
    } else {
        return FORMAT_NAME_ANDROID;
    }
}

int AndroidFormatWriter::init()
{
    if (!SHA1_Init(&_sha_ctx)) {
        _writer.set_error(AndroidError::Sha1InitError);
        return RET_FAILED;
    }

    return RET_OK;
}

int AndroidFormatWriter::get_header(File &file, Header &header)
{
    (void) file;

    header.set_supported_fields(SUPPORTED_FIELDS);

    return RET_OK;
}

int AndroidFormatWriter::write_header(File &file, const Header &header)
{
    int ret;

    // Construct header
    memset(&_hdr, 0, sizeof(_hdr));
    memcpy(_hdr.magic, BOOT_MAGIC, BOOT_MAGIC_SIZE);

    if (auto address = header.kernel_address()) {
        _hdr.kernel_addr = *address;
    }
    if (auto address = header.ramdisk_address()) {
        _hdr.ramdisk_addr = *address;
    }
    if (auto address = header.secondboot_address()) {
        _hdr.second_addr = *address;
    }
    if (auto address = header.kernel_tags_address()) {
        _hdr.tags_addr = *address;
    }
    if (auto page_size = header.page_size()) {
        switch (*page_size) {
        case 2048:
        case 4096:
        case 8192:
        case 16384:
        case 32768:
        case 65536:
        case 131072:
            _hdr.page_size = *page_size;
            break;
        default:
            _writer.set_error(AndroidError::InvalidPageSize,
                              "Invalid page size: %" PRIu32, *page_size);
            return RET_FAILED;
        }
    } else {
        _writer.set_error(AndroidError::MissingPageSize);
        return RET_FAILED;
    }

    if (auto board_name = header.board_name()) {
        if (board_name->size() >= sizeof(_hdr.name)) {
            _writer.set_error(AndroidError::BoardNameTooLong);
            return RET_FAILED;
        }

        strncpy(reinterpret_cast<char *>(_hdr.name), board_name->c_str(),
                sizeof(_hdr.name) - 1);
        _hdr.name[sizeof(_hdr.name) - 1] = '\0';
    }
    if (auto cmdline = header.kernel_cmdline()) {
        if (cmdline->size() >= sizeof(_hdr.cmdline)) {
            _writer.set_error(AndroidError::KernelCmdlineTooLong);
            return RET_FAILED;
        }

        strncpy(reinterpret_cast<char *>(_hdr.cmdline), cmdline->c_str(),
                sizeof(_hdr.cmdline) - 1);
        _hdr.cmdline[sizeof(_hdr.cmdline) - 1] = '\0';
    }

    // TODO: UNUSED
    // TODO: ID

    std::vector<SegmentWriterEntry> entries;

    entries.push_back({ ENTRY_TYPE_KERNEL, 0, {}, _hdr.page_size });
    entries.push_back({ ENTRY_TYPE_RAMDISK, 0, {}, _hdr.page_size });
    entries.push_back({ ENTRY_TYPE_SECONDBOOT, 0, {}, _hdr.page_size });
    entries.push_back({ ENTRY_TYPE_DEVICE_TREE, 0, {}, _hdr.page_size });

    ret = _seg.set_entries(_writer, std::move(entries));
    if (ret != RET_OK) { return ret; }

    // Start writing after first page
    auto seek_ret = file.seek(_hdr.page_size, SEEK_SET);
    if (!seek_ret) {
        _writer.set_error(seek_ret.error(),
                          "Failed to seek to first page: %s",
                          seek_ret.error().message().c_str());
        return file.is_fatal() ? RET_FATAL : RET_FAILED;
    }

    return RET_OK;
}

int AndroidFormatWriter::get_entry(File &file, Entry &entry)
{
    return _seg.get_entry(file, entry, _writer);
}

int AndroidFormatWriter::write_entry(File &file, const Entry &entry)
{
    return _seg.write_entry(file, entry, _writer);
}

int AndroidFormatWriter::write_data(File &file, const void *buf,
                                    size_t buf_size, size_t &bytes_written)
{
    int ret;

    ret = _seg.write_data(file, buf, buf_size, bytes_written, _writer);
    if (ret != RET_OK) {
        return ret;
    }

    // We always include the image in the hash. The size is sometimes included
    // and is handled in finish_entry().
    if (!SHA1_Update(&_sha_ctx, buf, buf_size)) {
        _writer.set_error(AndroidError::Sha1UpdateError);
        // This must be fatal as the write already happened and cannot be
        // reattempted
        return RET_FATAL;
    }

    return RET_OK;
}

int AndroidFormatWriter::finish_entry(File &file)
{
    int ret;

    ret = _seg.finish_entry(file, _writer);
    if (ret != RET_OK) {
        return ret;
    }

    auto swentry = _seg.entry();

    // Update SHA1 hash
    uint32_t le32_size = mb_htole32(*swentry->size);

    // Include size for everything except empty DT images
    if ((swentry->type != ENTRY_TYPE_DEVICE_TREE || *swentry->size > 0)
            && !SHA1_Update(&_sha_ctx, &le32_size, sizeof(le32_size))) {
        _writer.set_error(AndroidError::Sha1UpdateError);
        return RET_FATAL;
    }

    switch (swentry->type) {
    case ENTRY_TYPE_KERNEL:
        _hdr.kernel_size = *swentry->size;
        break;
    case ENTRY_TYPE_RAMDISK:
        _hdr.ramdisk_size = *swentry->size;
        break;
    case ENTRY_TYPE_SECONDBOOT:
        _hdr.second_size = *swentry->size;
        break;
    case ENTRY_TYPE_DEVICE_TREE:
        _hdr.dt_size = *swentry->size;
        break;
    }

    return RET_OK;
}

int AndroidFormatWriter::close(File &file)
{
    if (_file_size) {
        auto seek_ret = file.seek(static_cast<int64_t>(*_file_size), SEEK_SET);
        if (!seek_ret) {
            _writer.set_error(seek_ret.error(),
                              "Failed to seek to end of file: %s",
                              seek_ret.error().message().c_str());
            return file.is_fatal() ? RET_FATAL : RET_FAILED;
        }
    } else {
        auto file_size = file.seek(0, SEEK_CUR);
        if (!file_size) {
            _writer.set_error(file_size.error(),
                              "Failed to get file offset: %s",
                              file_size.error().message().c_str());
            return file.is_fatal() ? RET_FATAL : RET_FAILED;
        }

        _file_size = file_size.value();
    }

    auto swentry = _seg.entry();

    // If successful, finish up the boot image
    if (swentry == _seg.entries().end()) {
        // Write bump magic if we're outputting a bump'd image. Otherwise, write
        // the Samsung SEAndroid magic.
        if (_is_bump) {
            auto n = file_write_fully(file, bump::BUMP_MAGIC,
                                      bump::BUMP_MAGIC_SIZE);
            if (!n || n.value() != bump::BUMP_MAGIC_SIZE) {
                _writer.set_error(n.error(),
                                  "Failed to write Bump magic: %s",
                                  n.error().message().c_str());
                return file.is_fatal() ? RET_FATAL : RET_FAILED;
            }
        } else {
            auto n = file_write_fully(file, SAMSUNG_SEANDROID_MAGIC,
                                      SAMSUNG_SEANDROID_MAGIC_SIZE);
            if (!n || n.value() != SAMSUNG_SEANDROID_MAGIC_SIZE) {
                _writer.set_error(n.error(),
                                  "Failed to write SEAndroid magic: %s",
                                  n.error().message().c_str());
                return file.is_fatal() ? RET_FATAL : RET_FAILED;
            }
        }

        // Set ID
        unsigned char digest[SHA_DIGEST_LENGTH];
        if (!SHA1_Final(digest, &_sha_ctx)) {
            _writer.set_error(AndroidError::Sha1UpdateError);
            return RET_FATAL;
        }
        memcpy(_hdr.id, digest, SHA_DIGEST_LENGTH);

        // Convert fields back to little-endian
        AndroidHeader hdr = _hdr;
        android_fix_header_byte_order(hdr);

        // Seek back to beginning to write header
        auto seek_ret = file.seek(0, SEEK_SET);
        if (!seek_ret) {
            _writer.set_error(seek_ret.error(),
                              "Failed to seek to beginning: %s",
                              seek_ret.error().message().c_str());
            return file.is_fatal() ? RET_FATAL : RET_FAILED;
        }

        // Write header
        auto n = file_write_fully(file, &hdr, sizeof(hdr));
        if (!n || n.value() != sizeof(hdr)) {
            _writer.set_error(n.error(),
                              "Failed to write header: %s",
                              n.error().message().c_str());
            return file.is_fatal() ? RET_FATAL : RET_FAILED;
        }
    }

    return RET_OK;
}

}

/*!
 * \brief Set Android boot image output format
 *
 * \return
 *   * #RET_OK if the format is successfully set
 *   * \<= #RET_WARN if an error occurs
 */
int Writer::set_format_android()
{
    return register_format(
            std::make_unique<android::AndroidFormatWriter>(*this, false));
}

}
}
