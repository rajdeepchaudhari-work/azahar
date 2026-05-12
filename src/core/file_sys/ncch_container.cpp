// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <memory>
#include <span>
#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include <cryptopp/sha.h>
#include "core/file_sys/decrypted_romfs_reader.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/zstd_compression.h"
#include "core/core.h"
#include "core/file_sys/layered_fs.h"
#include "core/file_sys/ncch_container.h"
#include "core/file_sys/patch.h"
#include "core/file_sys/seed_db.h"
#include "core/hw/aes/key.h"
#include "core/hw/unique_data.h"
#include "core/loader/loader.h"

namespace FileSys {

static const int kMaxSections = 8;   ///< Maximum number of sections (files) in an ExeFs
static const int kBlockSize = 0x200; ///< Size of ExeFS blocks (in bytes)

u64 GetModId(u64 program_id) {
    constexpr u64 UPDATE_MASK = 0x0000000e'00000000;
    if ((program_id & 0x000000ff'00000000) == UPDATE_MASK) { // Apply the mods to updates
        return program_id & ~UPDATE_MASK;
    }
    return program_id;
}

/**
 * Get the decompressed size of an LZSS compressed ExeFS file
 * @param buffer Buffer of compressed file
 * @param size Size of compressed buffer
 * @return Size of decompressed buffer
 */
static std::size_t LZSS_GetDecompressedSize(std::span<const u8> buffer) {
    u32 offset_size;
    std::memcpy(&offset_size, buffer.data() + buffer.size() - sizeof(u32), sizeof(u32));
    return offset_size + buffer.size();
}

/**
 * Decompress ExeFS file (compressed with LZSS)
 * @param compressed Compressed buffer
 * @param compressed_size Size of compressed buffer
 * @param decompressed Decompressed buffer
 * @param decompressed_size Size of decompressed buffer
 * @return True on success, otherwise false
 */
static bool LZSS_Decompress(std::span<const u8> compressed, std::span<u8> decompressed) {
    const u8* footer = compressed.data() + compressed.size() - 8;

    u32 buffer_top_and_bottom;
    std::memcpy(&buffer_top_and_bottom, footer, sizeof(u32));

    std::size_t out = decompressed.size();
    std::size_t index = compressed.size() - ((buffer_top_and_bottom >> 24) & 0xFF);
    std::size_t stop_index = compressed.size() - (buffer_top_and_bottom & 0xFFFFFF);

    std::memset(decompressed.data(), 0, decompressed.size());
    std::memcpy(decompressed.data(), compressed.data(), compressed.size());

    while (index > stop_index) {
        u8 control = compressed[--index];

        for (unsigned i = 0; i < 8; i++) {
            if (index <= stop_index)
                break;
            if (index <= 0)
                break;
            if (out <= 0)
                break;

            if (control & 0x80) {
                // Check if compression is out of bounds
                if (index < 2)
                    return false;
                index -= 2;

                u32 segment_offset = compressed[index] | (compressed[index + 1] << 8);
                u32 segment_size = ((segment_offset >> 12) & 15) + 3;
                segment_offset &= 0x0FFF;
                segment_offset += 2;

                // Check if compression is out of bounds
                if (out < segment_size)
                    return false;

                for (unsigned j = 0; j < segment_size; j++) {
                    // Check if compression is out of bounds
                    if (out + segment_offset >= decompressed.size())
                        return false;

                    u8 data = decompressed[out + segment_offset];
                    decompressed[--out] = data;
                }
            } else {
                // Check if compression is out of bounds
                if (out < 1)
                    return false;
                decompressed[--out] = compressed[--index];
            }
            control <<= 1;
        }
    }
    return true;
}

NCCHContainer::NCCHContainer(const std::string& filepath, u32 ncch_offset, u32 partition)
    : ncch_offset(ncch_offset), partition(partition), filepath(filepath) {
    file = std::make_unique<FileUtil::IOFile>(filepath, "rb");
}

Loader::ResultStatus NCCHContainer::OpenFile(const std::string& filepath_, u32 ncch_offset_,
                                             u32 partition_) {
    filepath = filepath_;
    ncch_offset = ncch_offset_;
    partition = partition_;
    file = std::make_unique<FileUtil::IOFile>(filepath_, "rb");

    if (!file->IsOpen()) {
        LOG_WARNING(Service_FS, "Failed to open {}", filepath);
        return Loader::ResultStatus::Error;
    }

    LOG_DEBUG(Service_FS, "Opened {}", filepath);
    return Loader::ResultStatus::Success;
}

bool NCCHContainer::SetupDecryption() {
    using namespace HW::AES;
    InitKeys();

    if (ncch_header.fixed_key) {
        primary_key.fill(0);
        secondary_key.fill(0);
    } else {
        AESKey key_y_primary{};
        std::copy(std::begin(ncch_header.signature), std::begin(ncch_header.signature) + 16,
                  key_y_primary.begin());

        AESKey key_y_secondary = key_y_primary;
        if (ncch_header.seed_crypto) {
            auto opt = FileSys::GetSeed(ncch_header.program_id);
            if (!opt) {
                LOG_ERROR(Service_FS, "NCCH seed for {:016X} not found", ncch_header.program_id);
                return false;
            }
            std::array<u8, 32> seed_input{};
            std::memcpy(seed_input.data(),      key_y_primary.data(), 16);
            std::memcpy(seed_input.data() + 16, opt->data(),           16);
            std::array<u8, CryptoPP::SHA256::DIGESTSIZE> hash{};
            CryptoPP::SHA256().CalculateDigest(hash.data(), seed_input.data(), 32);
            std::memcpy(key_y_secondary.data(), hash.data(), 16);
        }

        SetKeyY(KeySlotID::NCCHSecure1, key_y_primary);
        if (!IsNormalKeyAvailable(KeySlotID::NCCHSecure1)) {
            LOG_ERROR(Service_FS, "Encrypted NCCH: slot 0x2C KeyX missing");
            return false;
        }
        primary_key = GetNormalKey(KeySlotID::NCCHSecure1);

        std::size_t secondary_slot = KeySlotID::NCCHSecure1;
        switch (ncch_header.secondary_key_slot) {
        case 0:  secondary_slot = KeySlotID::NCCHSecure1; break;
        case 1:  secondary_slot = KeySlotID::NCCHSecure2; break;
        case 10: secondary_slot = KeySlotID::NCCHSecure3; break;
        case 11: secondary_slot = KeySlotID::NCCHSecure4; break;
        default:
            LOG_ERROR(Service_FS, "Unknown secondary key slot {}", ncch_header.secondary_key_slot);
            return false;
        }
        SetKeyY(secondary_slot, key_y_secondary);
        if (!IsNormalKeyAvailable(secondary_slot)) {
            LOG_ERROR(Service_FS, "Encrypted NCCH: secondary KeyX missing for slot {:02X}",
                      secondary_slot);
            return false;
        }
        secondary_key = GetNormalKey(secondary_slot);
    }

    // Derive AES-CTR counters per section
    if (ncch_header.version == 0 || ncch_header.version == 2) {
        std::reverse_copy(std::begin(ncch_header.partition_id),
                          std::begin(ncch_header.partition_id) + 8,
                          exheader_ctr.begin());
        std::fill(exheader_ctr.begin() + 8, exheader_ctr.end(), 0);
        exefs_ctr   = exheader_ctr;
        romfs_ctr   = exheader_ctr;
        exheader_ctr[8] = 1;
        exefs_ctr[8]    = 2;
        romfs_ctr[8]    = 3;
    } else if (ncch_header.version == 1) {
        std::copy(std::begin(ncch_header.partition_id),
                  std::begin(ncch_header.partition_id) + 8,
                  exheader_ctr.begin());
        std::fill(exheader_ctr.begin() + 8, exheader_ctr.end(), 0);
        exefs_ctr = exheader_ctr;
        romfs_ctr = exheader_ctr;

        auto u32be = [](u32 v) {
            return std::array<u8, 4>{u8(v >> 24), u8((v >> 16) & 0xFF),
                                     u8((v >> 8) & 0xFF),  u8(v & 0xFF)};
        };
        auto off_exh  = u32be(0x200);
        auto off_exfs = u32be(static_cast<u32>(ncch_header.exefs_offset) * kBlockSize);
        auto off_rfs  = u32be(static_cast<u32>(ncch_header.romfs_offset) * kBlockSize);
        std::copy(off_exh.begin(),  off_exh.end(),  exheader_ctr.begin() + 12);
        std::copy(off_exfs.begin(), off_exfs.end(), exefs_ctr.begin()    + 12);
        std::copy(off_rfs.begin(),  off_rfs.end(),  romfs_ctr.begin()    + 12);
    } else {
        LOG_ERROR(Service_FS, "Unknown NCCH version {}", static_cast<u16>(ncch_header.version));
        return false;
    }

    is_encrypted = true;
    return true;
}

void NCCHContainer::DecryptBuffer(std::vector<u8>& buf, NCCHSection section,
                                  std::size_t offset_in_section) {
    if (buf.empty())
        return;

    const AESKey* key = nullptr;
    const AESKey* ctr = nullptr;

    switch (section) {
    case NCCHSection::ExHeader:
        key = &primary_key;   ctr = &exheader_ctr; break;
    case NCCHSection::ExeFS:
        key = &primary_key;   ctr = &exefs_ctr;    break;
    case NCCHSection::ExeFSSecondary:
        key = &secondary_key; ctr = &exefs_ctr;    break;
    case NCCHSection::RomFS:
        key = &secondary_key; ctr = &romfs_ctr;    break;
    }

    CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption d(key->data(), key->size(), ctr->data());
    if (offset_in_section != 0)
        d.Seek(offset_in_section);
    d.ProcessData(buf.data(), buf.data(), buf.size());
}

Loader::ResultStatus NCCHContainer::LoadHeader() {
    if (has_header) {
        return Loader::ResultStatus::Success;
    }

    if (!file || !file->IsOpen()) {
        return Loader::ResultStatus::Error;
    }

    if (FileUtil::Z3DSReadIOFile::GetUnderlyingFileMagic(file.get()) != std::nullopt) {
        // The file is compressed
        file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
    }

    for (int i = 0; i < 2; i++) {
        if (!file->IsOpen()) {
            return Loader::ResultStatus::Error;
        }

        // Reset read pointer in case this file has been read before.
        file->Seek(ncch_offset, SEEK_SET);

        if (file->ReadBytes(&ncch_header, sizeof(NCCH_Header)) != sizeof(NCCH_Header)) {
            return Loader::ResultStatus::Error;
        }

        // Skip NCSD header and load first NCCH (NCSD is just a container of NCCH files)...
        if (Loader::MakeMagic('N', 'C', 'S', 'D') == ncch_header.magic) {
            is_ncsd = true;
            NCSD_Header ncsd_header;
            file->Seek(ncch_offset, SEEK_SET);
            file->ReadBytes(&ncsd_header, sizeof(NCSD_Header));
            ASSERT(Loader::MakeMagic('N', 'C', 'S', 'D') == ncsd_header.magic);
            ASSERT(partition < 8);
            ncch_offset = ncsd_header.partitions[partition].offset * kBlockSize;
            file->Seek(ncch_offset, SEEK_SET);
            file->ReadBytes(&ncch_header, sizeof(NCCH_Header));
        }

        // Verify we are loading the correct file type...
        if (Loader::MakeMagic('N', 'C', 'C', 'H') != ncch_header.magic) {
            // We may be loading a crypto file, try again
            if (i == 0) {
                file = HW::UniqueData::OpenUniqueCryptoFile(
                    filepath, "rb", HW::UniqueData::UniqueCryptoFileID::NCCH);
                if (FileUtil::Z3DSReadIOFile::GetUnderlyingFileMagic(file.get()) != std::nullopt) {
                    // The file is compressed
                    file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
                }
            } else {
                return Loader::ResultStatus::ErrorInvalidFormat;
            }
        }
    }

    if (file->IsCrypto()) {
        LOG_DEBUG(Service_FS, "NCCH file has console unique crypto");
    }

    if (!ncch_header.no_crypto) {
        if (!SetupDecryption()) {
            return Loader::ResultStatus::ErrorEncrypted;
        }
    }

    has_header = true;
    return Loader::ResultStatus::Success;
}

Loader::ResultStatus NCCHContainer::Load() {
    if (is_loaded)
        return Loader::ResultStatus::Success;

    if (!file)
        return Loader::ResultStatus::Error;

    int block_size = kBlockSize;

    if (file->IsOpen()) {

        if (FileUtil::Z3DSReadIOFile::GetUnderlyingFileMagic(file.get()) != std::nullopt) {
            // The file is compressed
            file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
        }

        size_t file_size;
        for (int i = 0; i < 2; i++) {
            if (!file->IsOpen()) {
                return Loader::ResultStatus::Error;
            }

            file_size = file->GetSize();

            // Reset read pointer in case this file has been read before.
            file->Seek(ncch_offset, SEEK_SET);

            if (file->ReadBytes(&ncch_header, sizeof(NCCH_Header)) != sizeof(NCCH_Header))
                return Loader::ResultStatus::Error;

            // Skip NCSD header and load first NCCH (NCSD is just a container of NCCH files)...
            if (Loader::MakeMagic('N', 'C', 'S', 'D') == ncch_header.magic) {
                is_ncsd = true;
                NCSD_Header ncsd_header;
                file->Seek(ncch_offset, SEEK_SET);
                file->ReadBytes(&ncsd_header, sizeof(NCSD_Header));
                ASSERT(Loader::MakeMagic('N', 'C', 'S', 'D') == ncsd_header.magic);
                ASSERT(partition < 8);
                ncch_offset = ncsd_header.partitions[partition].offset * kBlockSize;
                file->Seek(ncch_offset, SEEK_SET);
                file->ReadBytes(&ncch_header, sizeof(NCCH_Header));
            }

            // Verify we are loading the correct file type...
            if (Loader::MakeMagic('N', 'C', 'C', 'H') != ncch_header.magic) {
                // We may be loading a crypto file, try again
                if (i == 0) {
                    file = HW::UniqueData::OpenUniqueCryptoFile(
                        filepath, "rb", HW::UniqueData::UniqueCryptoFileID::NCCH);
                    if (FileUtil::Z3DSReadIOFile::GetUnderlyingFileMagic(file.get()) !=
                        std::nullopt) {
                        // The file is compressed
                        file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
                    }
                } else {
                    return Loader::ResultStatus::ErrorInvalidFormat;
                }
            } else {
                break;
            }
        }

        if (file->IsCrypto()) {
            LOG_DEBUG(Service_FS, "NCCH file has console unique crypto");
        }
        if (file->IsCompressed()) {
            LOG_DEBUG(Service_FS, "NCCH file is compressed");
        }

        has_header = true;

        if (ncch_header.content_size == file_size) {
            // The NCCH is a proto version, which does not use media size units
            is_proto = true;
            block_size = 1;
        }

        if (!ncch_header.no_crypto && !is_encrypted) {
            if (!SetupDecryption()) {
                return Loader::ResultStatus::ErrorEncrypted;
            }
        }

        // System archives and DLC don't have an extended header but have RomFS
        // Proto apps don't have an ext header size
        if (ncch_header.extended_header_size || is_proto) {
            auto read_exheader = [this](FileUtil::IOFile* file) {
                const std::size_t size = sizeof(exheader_header);
                return file && file->ReadBytes(&exheader_header, size) == size;
            };

            if (!read_exheader(file.get())) {
                return Loader::ResultStatus::Error;
            }

            if (is_encrypted) {
                std::vector<u8> exh_buf(sizeof(ExHeader_Header));
                std::memcpy(exh_buf.data(), &exheader_header, sizeof(ExHeader_Header));
                DecryptBuffer(exh_buf, NCCHSection::ExHeader, 0);
                std::memcpy(&exheader_header, exh_buf.data(), sizeof(ExHeader_Header));
            }

            const auto mods_path =
                fmt::format("{}mods/{:016X}/", FileUtil::GetUserPath(FileUtil::UserPath::LoadDir),
                            GetModId(ncch_header.program_id));
            const std::array<std::string, 2> exheader_override_paths{{
                mods_path + "exheader.bin",
                filepath + ".exheader",
            }};

            bool has_exheader_override = false;
            for (const auto& path : exheader_override_paths) {
                FileUtil::IOFile exheader_override_file{path, "rb"};
                if (read_exheader(&exheader_override_file)) {
                    has_exheader_override = true;
                    break;
                }
            }
            if (has_exheader_override) {
                if (exheader_header.system_info.jump_id !=
                    exheader_header.arm11_system_local_caps.program_id) {
                    LOG_WARNING(Service_FS, "Jump ID and Program ID don't match. "
                                            "The override exheader might not be decrypted.");
                }
                is_tainted = true;
            }

            if (is_proto) {
                exheader_header.arm11_system_local_caps.priority = 0x30;
                exheader_header.arm11_system_local_caps.resource_limit_category = 0;
            }

            is_compressed = (exheader_header.codeset_info.flags.flag & 1) == 1;
            u32 entry_point = exheader_header.codeset_info.text.address;
            u32 code_size = exheader_header.codeset_info.text.code_size;
            u32 stack_size = exheader_header.codeset_info.stack_size;
            u32 bss_size = exheader_header.codeset_info.bss_size;
            u32 core_version = exheader_header.arm11_system_local_caps.core_version;
            u8 priority = exheader_header.arm11_system_local_caps.priority;
            u8 resource_limit_category =
                exheader_header.arm11_system_local_caps.resource_limit_category;

            LOG_DEBUG(Service_FS, "Name:                        {}",
                      reinterpret_cast<const char*>(exheader_header.codeset_info.name));
            LOG_DEBUG(Service_FS, "Program ID:                  {:016X}", ncch_header.program_id);
            LOG_DEBUG(Service_FS, "Code compressed:             {}", is_compressed ? "yes" : "no");
            LOG_DEBUG(Service_FS, "Entry point:                 0x{:08X}", entry_point);
            LOG_DEBUG(Service_FS, "Code size:                   0x{:08X}", code_size);
            LOG_DEBUG(Service_FS, "Stack size:                  0x{:08X}", stack_size);
            LOG_DEBUG(Service_FS, "Bss size:                    0x{:08X}", bss_size);
            LOG_DEBUG(Service_FS, "Core version:                {}", core_version);
            LOG_DEBUG(Service_FS, "Thread priority:             0x{:X}", priority);
            LOG_DEBUG(Service_FS, "Resource limit category:     {}", resource_limit_category);
            LOG_DEBUG(Service_FS, "System Mode:                 {}",
                      static_cast<int>(exheader_header.arm11_system_local_caps.system_mode));

            has_exheader = true;
        }

        // DLC can have an ExeFS and a RomFS but no extended header
        if (ncch_header.exefs_size) {
            exefs_offset = ncch_header.exefs_offset * block_size;
            u32 exefs_size = ncch_header.exefs_size * block_size;

            LOG_DEBUG(Service_FS, "ExeFS offset:                0x{:08X}", exefs_offset);
            LOG_DEBUG(Service_FS, "ExeFS size:                  0x{:08X}", exefs_size);

            file->Seek(exefs_offset + ncch_offset, SEEK_SET);
            if (file->ReadBytes(&exefs_header, sizeof(ExeFs_Header)) != sizeof(ExeFs_Header))
                return Loader::ResultStatus::Error;

            if (is_encrypted) {
                std::vector<u8> exfs_buf(sizeof(ExeFs_Header));
                std::memcpy(exfs_buf.data(), &exefs_header, sizeof(ExeFs_Header));
                DecryptBuffer(exfs_buf, NCCHSection::ExeFS, 0);
                std::memcpy(&exefs_header, exfs_buf.data(), sizeof(ExeFs_Header));
            }

            exefs_file = Reopen(file, filepath);

            has_exefs = true;
        }

        if (ncch_header.romfs_offset != 0 && ncch_header.romfs_size != 0)
            has_romfs = true;
    }

    LoadOverrides();

    // We need at least one of these or overrides, practically
    if (!(has_exefs || has_romfs || is_tainted))
        return Loader::ResultStatus::Error;

    is_loaded = true;
    return Loader::ResultStatus::Success;
}

Loader::ResultStatus NCCHContainer::LoadOverrides() {
    // Check for split-off files, mark the archive as tainted if we will use them
    std::string romfs_override = filepath + ".romfs";
    if (FileUtil::Exists(romfs_override)) {
        is_tainted = true;
    }

    // If we have a split-off exefs file/folder, it takes priority
    std::string exefs_override = filepath + ".exefs";
    std::string exefsdir_override = filepath + ".exefsdir/";
    if (FileUtil::Exists(exefs_override)) {
        exefs_file = std::make_unique<FileUtil::IOFile>(exefs_override, "rb");

        if (exefs_file->ReadBytes(&exefs_header, sizeof(ExeFs_Header)) == sizeof(ExeFs_Header)) {
            LOG_DEBUG(Service_FS, "Loading ExeFS section from {}", exefs_override);
            exefs_offset = 0;
            is_tainted = true;
            has_exefs = true;
        } else {
            exefs_file = Reopen(file, filepath);
        }
    } else if (FileUtil::Exists(exefsdir_override) && FileUtil::IsDirectory(exefsdir_override)) {
        is_tainted = true;
    }

    if (is_tainted)
        LOG_WARNING(Service_FS,
                    "Loaded NCCH {} is tainted, application behavior may not be as expected!",
                    filepath);

    return Loader::ResultStatus::Success;
}

Loader::ResultStatus NCCHContainer::LoadSectionExeFS(const char* name, std::vector<u8>& buffer) {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return result;

    int block_size = is_proto ? 1 : kBlockSize;

    // Proto has a different exefs format
    if (std::strcmp(name, ".code") == 0 && is_proto) {
        std::vector<u8> ro;
        std::vector<u8> rw;
        auto res = LoadSectionExeFS(".text", buffer);
        if (res != Loader::ResultStatus::Success) {
            return res;
        }
        res = LoadSectionExeFS(".ro", ro);
        if (res != Loader::ResultStatus::Success) {
            return res;
        }
        res = LoadSectionExeFS(".rw", rw);
        if (res != Loader::ResultStatus::Success) {
            return res;
        }
        buffer.insert(buffer.end(), ro.begin(), ro.end());
        buffer.insert(buffer.end(), rw.begin(), rw.end());
        return res;
    }

    // Check if we have files that can drop-in and replace
    result = LoadOverrideExeFSSection(name, buffer);
    if (result == Loader::ResultStatus::Success || !has_exefs)
        return result;

    // As of firmware 5.0.0-11 the logo is stored between the access descriptor and the plain region
    // instead of the ExeFS.
    if (std::strcmp(name, "logo") == 0) {
        if (ncch_header.logo_region_offset && ncch_header.logo_region_size) {
            std::size_t logo_offset = ncch_header.logo_region_offset * block_size;
            std::size_t logo_size = ncch_header.logo_region_size * block_size;

            buffer.resize(logo_size);
            file->Seek(ncch_offset + logo_offset, SEEK_SET);

            if (file->ReadBytes(buffer.data(), logo_size) != logo_size) {
                LOG_ERROR(Service_FS, "Could not read NCCH logo");
                return Loader::ResultStatus::Error;
            }
            return Loader::ResultStatus::Success;
        } else {
            LOG_INFO(Service_FS, "Attempting to load logo from the ExeFS");
        }
    }

    // If we don't have any separate files, we'll need a full ExeFS
    if (!exefs_file->IsOpen())
        return Loader::ResultStatus::Error;

    LOG_DEBUG(Service_FS, "{} sections:", kMaxSections);
    // Iterate through the ExeFs archive until we find a section with the specified name...
    for (unsigned section_number = 0; section_number < kMaxSections; section_number++) {
        const auto& section = exefs_header.section[section_number];

        // Load the specified section...
        if (strcmp(section.name, name) == 0) {
            LOG_DEBUG(Service_FS, "{} - offset: 0x{:08X}, size: 0x{:08X}, name: {}", section_number,
                      section.offset, section.size, section.name);

            s64 section_offset =
                is_proto ? section.offset
                         : (section.offset + exefs_offset + sizeof(ExeFs_Header) + ncch_offset);
            exefs_file->Seek(section_offset, SEEK_SET);

            size_t section_size = is_proto ? Common::AlignUp(section.size, 0x10) : section.size;

            if (strcmp(section.name, ".code") == 0 && is_compressed) {
                // Section is compressed, read compressed .code section...
                std::vector<u8> temp_buffer(section_size);
                if (exefs_file->ReadBytes(temp_buffer.data(), temp_buffer.size()) !=
                    temp_buffer.size())
                    return Loader::ResultStatus::Error;

                if (is_encrypted) {
                    // .code uses secondary key; offset = ExeFS header + section offset in stream
                    std::size_t stream_off = sizeof(ExeFs_Header) + section.offset;
                    DecryptBuffer(temp_buffer, NCCHSection::ExeFSSecondary, stream_off);
                }

                // Decompress .code section...
                buffer.resize(LZSS_GetDecompressedSize(temp_buffer));
                if (!LZSS_Decompress(temp_buffer, buffer)) {
                    return Loader::ResultStatus::ErrorInvalidFormat;
                }
            } else {
                // Section is uncompressed...
                buffer.resize(section_size);
                if (exefs_file->ReadBytes(buffer.data(), section_size) != section_size)
                    return Loader::ResultStatus::Error;

                if (is_encrypted) {
                    // "icon" and "banner" use primary key; everything else uses secondary key
                    bool use_primary = (strncmp(section.name, "icon",   4) == 0 ||
                                        strncmp(section.name, "banner", 6) == 0);
                    std::size_t stream_off = sizeof(ExeFs_Header) + section.offset;
                    DecryptBuffer(buffer,
                                  use_primary ? NCCHSection::ExeFS : NCCHSection::ExeFSSecondary,
                                  stream_off);
                }
            }

            return Loader::ResultStatus::Success;
        }
    }
    return Loader::ResultStatus::ErrorNotUsed;
}

Loader::ResultStatus NCCHContainer::ApplyCodePatch(std::vector<u8>& code) const {
    struct PatchLocation {
        std::string path;
        Loader::ResultStatus (*patch_fn)(const std::vector<u8>& patch, std::vector<u8>& code);
    };

    const auto mods_path =
        fmt::format("{}mods/{:016X}/", FileUtil::GetUserPath(FileUtil::UserPath::LoadDir),
                    GetModId(ncch_header.program_id));

    constexpr u32 system_module_tid_high = 0x00040130;

    std::string luma_ips_location;
    if ((static_cast<u32>(ncch_header.program_id >> 32) & system_module_tid_high) ==
        system_module_tid_high) {
        luma_ips_location =
            fmt::format("{}luma/sysmodules/{:016X}.ips",
                        FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir), ncch_header.program_id);
    } else {
        luma_ips_location =
            fmt::format("{}luma/titles/{:016X}/code.ips",
                        FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir), ncch_header.program_id);
    }
    const std::array<PatchLocation, 7> patch_paths{{
        {mods_path + "exefs/code.ips", Patch::ApplyIpsPatch},
        {mods_path + "exefs/code.bps", Patch::ApplyBpsPatch},
        {mods_path + "code.ips", Patch::ApplyIpsPatch},
        {mods_path + "code.bps", Patch::ApplyBpsPatch},
        {luma_ips_location, Patch::ApplyIpsPatch},
        {filepath + ".exefsdir/code.ips", Patch::ApplyIpsPatch},
        {filepath + ".exefsdir/code.bps", Patch::ApplyBpsPatch},
    }};

    for (const PatchLocation& info : patch_paths) {
        FileUtil::IOFile patch_file{info.path, "rb"};
        if (!patch_file)
            continue;

        std::vector<u8> patch(patch_file.GetSize());
        if (patch_file.ReadBytes(patch.data(), patch.size()) != patch.size())
            return Loader::ResultStatus::ErrorPatches;

        LOG_INFO(Service_FS, "File {} patching code.bin", info.path);
        auto patch_result = info.patch_fn(patch, code);
        if (patch_result != Loader::ResultStatus::Success)
            return patch_result;

        return Loader::ResultStatus::Success;
    }
    return Loader::ResultStatus::ErrorNotUsed;
}

Loader::ResultStatus NCCHContainer::LoadOverrideExeFSSection(const char* name,
                                                             std::vector<u8>& buffer) {
    std::string override_name;

    // Map our section name to the extracted equivalent
    if (!strcmp(name, ".code"))
        override_name = "code.bin";
    else if (!strcmp(name, "icon"))
        override_name = "icon.bin";
    else if (!strcmp(name, "banner"))
        override_name = "banner.bnr";
    else if (!strcmp(name, "logo"))
        override_name = "logo.bcma.lz";
    else
        return Loader::ResultStatus::Error;

    const auto mods_path =
        fmt::format("{}mods/{:016X}/", FileUtil::GetUserPath(FileUtil::UserPath::LoadDir),
                    GetModId(ncch_header.program_id));
    const std::array<std::string, 3> override_paths{{
        mods_path + "exefs/" + override_name,
        mods_path + override_name,
        filepath + ".exefsdir/" + override_name,
    }};

    for (const auto& path : override_paths) {
        FileUtil::IOFile section_file(path, "rb");

        if (section_file.IsOpen()) {
            auto section_size = section_file.GetSize();
            buffer.resize(section_size);

            section_file.Seek(0, SEEK_SET);
            if (section_file.ReadBytes(buffer.data(), section_size) == section_size) {
                LOG_WARNING(Service_FS, "File {} overriding built-in ExeFS file", path);
                return Loader::ResultStatus::Success;
            }
        }
    }
    return Loader::ResultStatus::ErrorNotUsed;
}

Loader::ResultStatus NCCHContainer::ReadRomFS(std::shared_ptr<RomFSReader>& romfs_file,
                                              bool use_layered_fs) {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return result;

    int block_size = is_proto ? 1 : kBlockSize;

    if (ReadOverrideRomFS(romfs_file) == Loader::ResultStatus::Success)
        return Loader::ResultStatus::Success;

    if (!has_romfs) {
        LOG_DEBUG(Service_FS, "RomFS requested from NCCH which has no RomFS");
        return Loader::ResultStatus::ErrorNotUsed;
    }

    if (!file || !file->IsOpen())
        return Loader::ResultStatus::Error;

    u32 romfs_offset = ncch_offset + (ncch_header.romfs_offset * block_size) + 0x1000;
    u32 romfs_size = (ncch_header.romfs_size * block_size) - 0x1000;

    LOG_DEBUG(Service_FS, "RomFS offset:           0x{:08X}", romfs_offset);
    LOG_DEBUG(Service_FS, "RomFS size:             0x{:08X}", romfs_size);

    if (file->GetSize() < romfs_offset + romfs_size)
        return Loader::ResultStatus::Error;

    // We reopen the file, to allow its position to be independent from file's
    std::unique_ptr<FileUtil::IOFile> romfs_file_inner;
    romfs_file_inner = Reopen(file, filepath);

    if (!romfs_file_inner->IsOpen())
        return Loader::ResultStatus::Error;

    std::shared_ptr<RomFSReader> direct_romfs =
        std::make_shared<DirectRomFSReader>(std::move(romfs_file_inner), romfs_offset, romfs_size);

    if (is_encrypted) {
        // stream_offset = 0x1000: the IVFC header is 0x1000 bytes into the RomFS section's
        // CTR stream; the data read by direct_romfs starts right after that header.
        direct_romfs = std::make_shared<DecryptedRomFSReader>(
            std::move(direct_romfs), secondary_key, romfs_ctr, 0x1000);
    }

    const auto path =
        fmt::format("{}mods/{:016X}/", FileUtil::GetUserPath(FileUtil::UserPath::LoadDir),
                    GetModId(ncch_header.program_id));
    if (!is_proto && use_layered_fs &&
        (FileUtil::Exists(path + "romfs/") || FileUtil::Exists(path + "romfs_ext/"))) {

        romfs_file = std::make_shared<LayeredFS>(std::move(direct_romfs), path + "romfs/",
                                                 path + "romfs_ext/");
    } else {
        romfs_file = std::move(direct_romfs);
    }

    return Loader::ResultStatus::Success;
}

Loader::ResultStatus NCCHContainer::DumpRomFS(const std::string& target_path) {
    if (file->IsCrypto())
        return Loader::ResultStatus::ErrorEncrypted;

    std::shared_ptr<RomFSReader> direct_romfs;
    Loader::ResultStatus result = ReadRomFS(direct_romfs, false);
    if (result != Loader::ResultStatus::Success)
        return result;

    std::shared_ptr<LayeredFS> layered_fs =
        std::make_shared<LayeredFS>(std::move(direct_romfs), "", "", false);

    if (!layered_fs->DumpRomFS(target_path)) {
        return Loader::ResultStatus::Error;
    }
    return Loader::ResultStatus::Success;
}

Loader::ResultStatus NCCHContainer::ReadOverrideRomFS(std::shared_ptr<RomFSReader>& romfs_file) {
    // Check for RomFS overrides
    std::string split_filepath = filepath + ".romfs";
    if (FileUtil::Exists(split_filepath)) {
        std::unique_ptr<FileUtil::IOFile> romfs_file_inner =
            std::make_unique<FileUtil::IOFile>(split_filepath, "rb");
        if (romfs_file_inner->IsOpen()) {
            LOG_WARNING(Service_FS, "File {} overriding built-in RomFS; LayeredFS not enabled",
                        split_filepath);
            romfs_file = std::make_shared<DirectRomFSReader>(std::move(romfs_file_inner), 0,
                                                             romfs_file_inner->GetSize());
            return Loader::ResultStatus::Success;
        }
    }

    return Loader::ResultStatus::ErrorNotUsed;
}

Loader::ResultStatus NCCHContainer::ReadProgramId(u64_le& program_id) {
    Loader::ResultStatus result = LoadHeader();
    if (result != Loader::ResultStatus::Success)
        return result;

    if (!has_header)
        return Loader::ResultStatus::ErrorNotUsed;

    program_id = ncch_header.program_id;
    return Loader::ResultStatus::Success;
}

Loader::ResultStatus NCCHContainer::ReadExtdataId(u64& extdata_id) {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return result;

    if (!has_exheader)
        return Loader::ResultStatus::ErrorNotUsed;

    if (exheader_header.arm11_system_local_caps.storage_info.other_attributes >> 1) {
        // Using extended save data access
        // There would be multiple possible extdata IDs in this case. The best we can do for now is
        // guessing that the first one would be the main save.
        const std::array<u64, 6> extdata_ids{{
            exheader_header.arm11_system_local_caps.storage_info.extdata_id0.Value(),
            exheader_header.arm11_system_local_caps.storage_info.extdata_id1.Value(),
            exheader_header.arm11_system_local_caps.storage_info.extdata_id2.Value(),
            exheader_header.arm11_system_local_caps.storage_info.extdata_id3.Value(),
            exheader_header.arm11_system_local_caps.storage_info.extdata_id4.Value(),
            exheader_header.arm11_system_local_caps.storage_info.extdata_id5.Value(),
        }};
        for (u64 id : extdata_ids) {
            if (id) {
                // Found a non-zero ID, use it
                extdata_id = id;
                return Loader::ResultStatus::Success;
            }
        }

        return Loader::ResultStatus::ErrorNotUsed;
    }

    extdata_id = exheader_header.arm11_system_local_caps.storage_info.ext_save_data_id;
    return Loader::ResultStatus::Success;
}

bool NCCHContainer::HasExeFS() {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return false;

    return has_exefs;
}

bool NCCHContainer::HasRomFS() {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return false;

    return has_romfs;
}

bool NCCHContainer::HasExHeader() {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return false;

    return has_exheader;
}

std::unique_ptr<FileUtil::IOFile> NCCHContainer::Reopen(
    const std::unique_ptr<FileUtil::IOFile>& orig_file, const std::string& new_filename) {
    if (!orig_file)
        return nullptr;

    const bool is_compressed = orig_file->IsCompressed();
    const bool is_crypto = orig_file->IsCrypto();
    const std::string filename = new_filename.empty() ? orig_file->Filename() : new_filename;

    std::unique_ptr<FileUtil::IOFile> out_file;
    if (is_crypto) {
        out_file = HW::UniqueData::OpenUniqueCryptoFile(filename, "rb",
                                                        HW::UniqueData::UniqueCryptoFileID::NCCH);
    } else {
        out_file = std::make_unique<FileUtil::IOFile>(filename, "rb");
    }
    if (is_compressed) {
        out_file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(out_file));
    }

    return out_file;
}

} // namespace FileSys
