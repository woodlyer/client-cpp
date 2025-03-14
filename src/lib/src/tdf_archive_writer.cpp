/*
* Copyright 2022 Virtru Corporation
*
* SPDX - License Identifier: BSD-3-Clause-Clear
*
*/
//
//  TDF SDK
//
//  Created by Sujan Reddy on 2022/05/16
//

#include <chrono>
#include <vector>
#include <zlib.h>

#include "tdf_archive_writer.h"
#include "tdf_exception.h"

namespace virtru {

    constexpr auto InternalBufferSIze = 2 * 1024 * 1024;

    /// Constructor
    TDFArchiveWriter::TDFArchiveWriter(IOutputProvider *outputProvider,
                                       std::string manifestFilename,
                                       std::string payloadFileName)
            : m_outputProvider(outputProvider),
              m_manifestFilename(manifestFilename),
              m_payloadFilename(payloadFileName) {

        // Allocate enough memory
        m_buffer.resize(InternalBufferSIze);
    }

    void GetTimeDate(std::time_t rawtime, uint16_t & fileTime, uint16_t & fileDate)
    {
        struct tm time;
#ifdef _WIN32
        gmtime_s(&time, &rawtime);
#else
        gmtime_r(&rawtime, &time);
#endif
        fileTime = static_cast<uint16_t>(time.tm_hour << 11 | time.tm_min << 5 | std::max(time.tm_sec / 2, 29));
        fileDate = static_cast<uint16_t>((time.tm_year - 80) << 9 | (time.tm_mon + 1) << 5 | time.tm_mday);
    }

    /// Append the payload contents to the archive.
    void TDFArchiveWriter::appendPayload(Bytes payload) {
        LogTrace("TDFArchiveWriter::appendPayload");
        if(m_payloadSize > ZIP64_MAGICVAL)
            m_isZip64 = true;

        LocalFileHeader lfh{};
        uint16_t fileTime = 0;
        uint16_t fileDate = 0;
        GetTimeDate(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()), fileTime, fileDate);

        if (PayloadState::Initial == m_payloadState) {
            lfh.signature = static_cast<uint32_t>(ZipSignatures::LocalFileHeaderSignature);
            lfh.version = 45;
            //since payload is added by chunks we set General purpose bit flag to 0x08 (https://en.wikipedia.org/wiki/ZIP_(file_format))
            lfh.flags = 0x08;

            lfh.compressionMethod = 0;
            lfh.lastModifiedTime = fileTime;
            lfh.lastModifiedDate = fileDate;
            lfh.crc32 = 0;

            if (m_isZip64) {
                lfh.compressedSize = ZIP64_MAGICVAL;
                lfh.uncompressedSize = ZIP64_MAGICVAL;
                lfh.extraFieldLength = sizeof(Zip64ExtendedLocalInfoExtraField);
            }
            else {
                lfh.compressedSize = 0;
                lfh.uncompressedSize = 0;
                lfh.extraFieldLength = 0;
            }

            lfh.filenameLength = m_payloadFilename.length();

            // Write LFH Structure
            WriteableBytes bytes = allotMemory(sizeof(lfh));
            std::memcpy(bytes.data(), &lfh, sizeof(lfh));
            m_outputProvider->writeBytes(bytes);

            //Write filename
            bytes = allotMemory(m_payloadFilename.length());
            std::memcpy(bytes.data(), m_payloadFilename.c_str(), m_payloadFilename.length());
            m_outputProvider->writeBytes(bytes);

            if (m_isZip64) {
                Zip64ExtendedLocalInfoExtraField zip64ExtendedLocalInfo {};
                zip64ExtendedLocalInfo.signature = ZIP64_EXTID;
                zip64ExtendedLocalInfo.size = sizeof(Zip64ExtendedLocalInfoExtraField) - 4;
                zip64ExtendedLocalInfo.originalSize = m_payloadSize;
                zip64ExtendedLocalInfo.compressedSize = m_payloadSize;

                bytes = allotMemory(sizeof(zip64ExtendedLocalInfo));
                std::memcpy(bytes.data(), &zip64ExtendedLocalInfo, sizeof(zip64ExtendedLocalInfo));
                m_outputProvider->writeBytes(bytes);
            }

            m_payloadState = PayloadState::Appending;
        }


        //Write payload content
        m_outputProvider->writeBytes(payload);
        static uint32_t computed_crc32_ {0};
        computed_crc32_ = crc32(computed_crc32_, reinterpret_cast<const unsigned char *>(payload.data()),
                                                      payload.size());
        static size_t current_size;
        current_size += payload.size();

        if (current_size >= m_payloadSize) {
            m_payloadState = PayloadState::Finished;
            m_fileInfo.emplace_back(FileInfo{ computed_crc32_, m_payloadSize,  m_currentOffset, m_payloadFilename, fileTime, fileDate, 0x08});
        }

        if (PayloadState::Finished == m_payloadState) {
            // Write Data descriptor
            if (m_isZip64) {
                DataDescriptor64 dd64{};
                dd64.signature = static_cast<uint32_t>(ZipSignatures::DataDescriptorSignature);
                dd64.crc32 = computed_crc32_;
                dd64.compressedSize = m_payloadSize;
                dd64.uncompressedSize = m_payloadSize;

                WriteableBytes bytes = allotMemory(sizeof(dd64));
                std::memcpy(bytes.data(), &dd64, sizeof(dd64));
                m_outputProvider->writeBytes(bytes);
                m_currentOffset = sizeof(LocalFileHeader) + m_payloadFilename.length() + m_payloadSize + sizeof(DataDescriptor64) + sizeof(Zip64ExtendedLocalInfoExtraField);
            }
            else {
                DataDescriptor32 dd32{};
                dd32.signature = static_cast<uint32_t>(ZipSignatures::DataDescriptorSignature);
                dd32.crc32 = computed_crc32_;
                dd32.compressedSize = m_payloadSize;
                dd32.uncompressedSize = m_payloadSize;

                WriteableBytes bytes = allotMemory(sizeof(dd32));
                std::memcpy(bytes.data(), &dd32, sizeof(dd32));
                m_outputProvider->writeBytes(bytes);
                m_currentOffset = sizeof(LocalFileHeader) + m_payloadFilename.length() + m_payloadSize + sizeof(DataDescriptor32);
            }

            current_size = 0;
            computed_crc32_ = 0;
        }
    }

    /// Append the manifest contents to the archive.
    void TDFArchiveWriter::appendManifest(ManifestDataModel manifestDataModel) {

        m_manifest = manifestDataModel.toJson();

        LogTrace("TDFArchiveWriter::appendManifest");
        LocalFileHeader lfh{};
        lfh.signature = static_cast<uint32_t>(ZipSignatures::LocalFileHeaderSignature);
        lfh.version = 45;
        lfh.flags = 0;
        uint16_t fileTime = 0;
        uint16_t fileDate = 0;
        GetTimeDate(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()), fileTime, fileDate);
        lfh.compressionMethod = 0;
        lfh.lastModifiedTime = fileTime;
        lfh.lastModifiedDate = fileDate;
        lfh.crc32 = crc32(0, reinterpret_cast<uint8_t *>(m_manifest.data()), m_manifest.size());;
        lfh.compressedSize = m_manifest.size();
        lfh.uncompressedSize = m_manifest.size();
        lfh.filenameLength = m_manifestFilename.length();
        if (m_isZip64) {
            lfh.compressedSize = ZIP64_MAGICVAL;
            lfh.uncompressedSize = ZIP64_MAGICVAL;
            lfh.extraFieldLength = sizeof(Zip64ExtendedLocalInfoExtraField);
        }
        else {
            lfh.compressedSize = m_manifest.size();
            lfh.uncompressedSize = m_manifest.size();
            lfh.extraFieldLength = 0;
        }

        // Write LFH Structure
        WriteableBytes bytes = allotMemory(sizeof(lfh));
        std::memcpy(bytes.data(), &lfh, sizeof(lfh));
        m_outputProvider->writeBytes(bytes);

        //Write filename
        auto fileNameLen = m_manifestFilename.length();
        bytes = allotMemory(fileNameLen);
        std::memcpy(bytes.data(), m_manifestFilename.c_str(), m_manifestFilename.length());
        m_outputProvider->writeBytes(bytes);

        if (m_isZip64) {
            Zip64ExtendedLocalInfoExtraField zip64ExtendedLocalInfo{};
            zip64ExtendedLocalInfo.signature = ZIP64_EXTID;
            zip64ExtendedLocalInfo.size = sizeof(Zip64ExtendedLocalInfoExtraField) - 4;
            zip64ExtendedLocalInfo.originalSize = m_manifest.size();;
            zip64ExtendedLocalInfo.compressedSize = m_manifest.size();;

            bytes = allotMemory(sizeof(zip64ExtendedLocalInfo));
            std::memcpy(bytes.data(), &zip64ExtendedLocalInfo, sizeof(zip64ExtendedLocalInfo));
            m_outputProvider->writeBytes(bytes);
        }

        //Write manifest content
        m_outputProvider->writeBytes(toBytes(m_manifest));
        m_fileInfo.emplace_back(FileInfo{lfh.crc32, m_manifest.size(),
                                         m_currentOffset, m_manifestFilename,
                                         fileTime, fileDate, 0x0});

        m_currentOffset += sizeof(LocalFileHeader) + m_manifestFilename.length() + m_manifest.size();
        if (m_isZip64)
            m_currentOffset += sizeof(Zip64ExtendedLocalInfoExtraField);
    }

    void TDFArchiveWriter::writeCentralDirectory() {
        LogTrace("TDFArchiveWriter::writeCentralDirectory");
        m_lastOffsetCDFH = m_currentOffset;
        for(unsigned int i = 0; i < m_fileInfo.size(); ++i) {
            CentralDirectoryFileHeader cdfh{};
            const FileInfo &fileInfo = m_fileInfo[i];
            cdfh.signature = static_cast<uint32_t>(ZipSignatures::CentralFileHeaderSignature);
            cdfh.versionCreated = 45;
            cdfh.versionNeeded = 45;
            cdfh.flags = fileInfo.flag;
            cdfh.compressionMethod = 0;
            cdfh.lastModifiedTime = fileInfo.fileTime;
            cdfh.lastModifiedDate = fileInfo.fileDate;
            cdfh.crc32 = fileInfo.crc;
            cdfh.filenameLength = fileInfo.fileName.size();
            cdfh.fileCommentLength = 0;
            cdfh.diskNumberStart = 0;
            cdfh.internalFileAttributes = 0;
            cdfh.externalFileAttributes = 0;

            if (m_isZip64) {
                cdfh.compressedSize = ZIP64_MAGICVAL;
                cdfh.uncompressedSize = ZIP64_MAGICVAL;
                cdfh.localHeaderOffset = ZIP64_MAGICVAL;
                cdfh.extraFieldLength = sizeof(Zip64ExtendedInfoExtraField);
            }
            else {
                cdfh.compressedSize = fileInfo.size;
                cdfh.uncompressedSize = fileInfo.size;
                cdfh.localHeaderOffset = fileInfo.offset;
                cdfh.extraFieldLength = 0;
            }

            // Write CDFH Structure
            auto bytes = allotMemory(sizeof(cdfh));
            std::memcpy(bytes.data(), &cdfh, sizeof(cdfh));
            m_outputProvider->writeBytes(bytes);

            //Write filename
            m_outputProvider->writeBytes(toBytes(fileInfo.fileName));

            if (m_isZip64) {
                Zip64ExtendedInfoExtraField zip64ExtendedInfo{};
                zip64ExtendedInfo.signature = ZIP64_EXTID;
                zip64ExtendedInfo.size = sizeof(Zip64ExtendedInfoExtraField) - 4;
                zip64ExtendedInfo.originalSize = fileInfo.size;
                zip64ExtendedInfo.compressedSize = fileInfo.size;
                zip64ExtendedInfo.localFileHeaderOffset = fileInfo.offset;

                bytes = allotMemory(sizeof(Zip64ExtendedInfoExtraField));
                std::memcpy(bytes.data(), &zip64ExtendedInfo, sizeof(zip64ExtendedInfo));
                m_outputProvider->writeBytes(bytes);
            }
            m_lastOffsetCDFH +=  sizeof(CentralDirectoryFileHeader) + fileInfo.fileName.size();
            if (m_isZip64)
                m_lastOffsetCDFH += sizeof(Zip64ExtendedInfoExtraField);
        }
    }

    void TDFArchiveWriter::writeEndOfCentralDirectory() {
        LogTrace("TDFArchiveWriter::writeEndOfCentralDirectory");
        if (m_isZip64) {
            writeZip64EndOfCentralDirectory();
            writeZip64EndOfCentralDirectoryLocator();
        }
        EndOfCentralDirectoryRecord eocd{};
        eocd.signature = static_cast<uint32_t>(ZipSignatures::EndOfCentralDirectorySignature);
        eocd.diskNumber = 0;
        eocd.startDiskNumber = 0;
        eocd.centralDirectoryOffset = m_currentOffset;
        eocd.numberCentralDirectoryRecord = m_fileInfo.size();
        eocd.totalCentralDirectoryRecord = m_fileInfo.size();
        eocd.sizeOfCentralDirectory = m_lastOffsetCDFH - m_currentOffset;
        eocd.commentLength = 0;

        if (m_isZip64)
            eocd.centralDirectoryOffset = ZIP64_MAGICVAL;

        // Write EOCD Structure
        auto bytes = allotMemory(sizeof(eocd));
        std::memcpy(bytes.data(), &eocd, sizeof(eocd));
        m_outputProvider->writeBytes(bytes);
    }

    void TDFArchiveWriter::writeZip64EndOfCentralDirectory() {
        LogTrace("TDFArchiveWriter::writeZip64EndOfCentralDirectory");
        Zip64EndOfCentralDirectoryRecord zip64eocd{};
        zip64eocd.signature = static_cast<uint32_t>(ZipSignatures::Zip64EndOfCentralDirectorySignature);
        zip64eocd.recordSize = sizeof(zip64eocd) - 12;
        zip64eocd.versionMadeBy = 45;
        zip64eocd.versionToExtract = 45;
        zip64eocd.diskNumber = 0;
        zip64eocd.centralDirectoryDiskNumber = 0;
        zip64eocd.entryCountThisDisk = m_fileInfo.size();
        zip64eocd.totalEntryCount = m_fileInfo.size();
        zip64eocd.centralDirectorySize = m_lastOffsetCDFH - m_currentOffset;;
        zip64eocd.startingDiskCentralDirectoryOffset = m_currentOffset;

        // Write Zip64EndOfCentralDirectory Structure
        auto bytes = allotMemory(sizeof(zip64eocd));
        std::memcpy(bytes.data(), &zip64eocd, sizeof(zip64eocd));
        m_outputProvider->writeBytes(bytes);
    }

    void TDFArchiveWriter::writeZip64EndOfCentralDirectoryLocator() {
        LogTrace("TDFArchiveWriter::writeZip64EndOfCentralDirectoryLocator");
        Zip64EndOfCentralDirectoryRecordLocator zip64eocdlocator{};
        zip64eocdlocator.signature = static_cast<uint32_t>(ZipSignatures::Zip64EndOfCentralDirectoryLocatorSignature);
        zip64eocdlocator.centralDirectoryStartDiskNumber = 0;
        zip64eocdlocator.centralDirectoryOffset = m_lastOffsetCDFH;
        zip64eocdlocator.numberOfDisks = 1;

        // Write Zip64EndOfCentralDirectoryLocator Structure
        auto bytes = allotMemory(sizeof(zip64eocdlocator));
        std::memcpy(bytes.data(), &zip64eocdlocator, sizeof(zip64eocdlocator));
        m_outputProvider->writeBytes(bytes);
    }

    void TDFArchiveWriter::finish() {
        writeCentralDirectory();
        writeEndOfCentralDirectory();
        m_outputProvider->flush();
    }

    /// Return the manifest stored in TDF
    std::string TDFArchiveWriter::getManifest() const {
        return m_manifest;
    }

    /// Allot memory of size.
    WriteableBytes TDFArchiveWriter::allotMemory(size_t size) {
        if (m_buffer.size() < size) {
            m_buffer.resize(size);
        }

        return WriteableBytes{m_buffer.data(), m_buffer.data() + size};
    }

}