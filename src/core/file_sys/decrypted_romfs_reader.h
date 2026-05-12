// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include "common/common_types.h"
#include "core/file_sys/romfs_reader.h"

namespace FileSys {

// Wraps a RomFSReader and decrypts data on-the-fly using AES-CTR.
// stream_offset is the byte position within the CTR stream that corresponds
// to offset 0 of the wrapped reader (i.e. the IVFC header size, 0x1000).
class DecryptedRomFSReader final : public RomFSReader {
public:
    using AESKey = std::array<u8, 16>;

    DecryptedRomFSReader(std::shared_ptr<RomFSReader> base, const AESKey& key, const AESKey& ctr,
                         std::size_t stream_offset)
        : base(std::move(base)), key(key), ctr(ctr), stream_offset(stream_offset) {}

    ~DecryptedRomFSReader() override = default;

    std::size_t GetSize() const override {
        return base->GetSize();
    }

    std::size_t ReadFile(std::size_t offset, std::size_t length, u8* buffer) override {
        std::size_t read = base->ReadFile(offset, length, buffer);
        if (read > 0) {
            CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption d(key.data(), key.size(), ctr.data());
            d.Seek(stream_offset + offset);
            d.ProcessData(buffer, buffer, read);
        }
        return read;
    }

    bool AllowsCachedReads() const override {
        return false;
    }

    bool CacheReady(std::size_t /*file_offset*/, std::size_t /*length*/) override {
        return false;
    }

private:
    std::shared_ptr<RomFSReader> base;
    AESKey key;
    AESKey ctr;
    std::size_t stream_offset;
};

} // namespace FileSys
