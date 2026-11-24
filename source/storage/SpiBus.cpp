#if defined(ARDUINO_ARCH_ESP32) && defined(HAS_SDCARD) && !defined(HAS_SD_MMC) && !defined(SDCARD_USE_SOFT_SPI)

#include "SD.h"
#include "SPI.h"
#include "diskio.h"
#include "diskio_impl.h"
#include "ff.h"
#include "sd_diskio.h"
#include "storage/FatFsStorage.h"
#include "util/ILog.h"

#ifndef SD_SPI_FREQUENCY
#define SD_SPI_FREQUENCY 50000000
#endif

#ifdef SDCARD_USE_SPI1
extern SPIClass SPI_HSPI;
static SPIClass &SDHandler = SPI_HSPI;
#else
static SPIClass &SDHandler = SPI;
#endif

namespace
{
// the firmware mounts the card with SD.h at the same place, so both share one mount
constexpr const char *VFS_PATH = "/sd";
constexpr uint8_t MAX_FILES = 5;

// SDFS keeps its FatFs drive number protected
struct SdDrive : fs::SDFS {
    using fs::SDFS::_pdrv;
};

} // namespace

// the card functions of the SD library; it has no header for them
DSTATUS ff_sd_initialize(uint8_t pdrv);
DSTATUS ff_sd_status(uint8_t pdrv);
DRESULT ff_sd_read(uint8_t pdrv, uint8_t *buffer, DWORD sector, UINT count);
DRESULT ff_sd_write(uint8_t pdrv, const uint8_t *buffer, DWORD sector, UINT count);
DRESULT ff_sd_ioctl(uint8_t pdrv, uint8_t cmd, void *buff);

namespace
{
/**
 * The SD library asks the card for its status right after the last block of a write, without waiting
 * for the card to start or finish programming, and gives the whole write up on one bad answer
 * ("token error [13]"). That happens once in a few megabytes and ends a long write for good, as FatFs
 * keeps the error in the file. Sectors can be written twice without harm, so these try again after the
 * card had time to settle.
 */
constexpr int c_tries = 4;

DSTATUS retryStatus(uint8_t pdrv)
{
    DSTATUS status = ff_sd_status(pdrv);
    for (int i = 1; i < c_tries && (status & STA_NOINIT); i++) {
        delay(20 * i);
        status = ff_sd_status(pdrv);
    }
    return status;
}

DRESULT retryRead(uint8_t pdrv, uint8_t *buffer, uint32_t sector, unsigned count)
{
    DRESULT result = ff_sd_read(pdrv, buffer, sector, count);
    for (int i = 1; i < c_tries && result == RES_ERROR; i++) {
        delay(20 * i);
        result = ff_sd_read(pdrv, buffer, sector, count);
    }
    return result;
}

DRESULT retryWrite(uint8_t pdrv, const uint8_t *buffer, uint32_t sector, unsigned count)
{
    DRESULT result = ff_sd_write(pdrv, buffer, sector, count);
    for (int i = 1; i < c_tries && result == RES_ERROR; i++) {
        ILOG_WARN("card write of %u sectors at %u failed, try %d", count, (unsigned)sector, i + 1);
        delay(20 * i);
        result = ff_sd_write(pdrv, buffer, sector, count);
    }
    return result;
}

void patientCard(uint8_t pdrv)
{
    static const ff_diskio_impl_t impl = {
        .init = &ff_sd_initialize, .status = &retryStatus, .read = &retryRead, .write = &retryWrite, .ioctl = &ff_sd_ioctl};
    if (pdrv != 0xFF) {
        ff_diskio_register(pdrv, &impl);
        ILOG_INFO("card on drive %u: reads and writes are tried again when they fail", pdrv);
    }
}

/**
 * Card on a hardware SPI bus, driven by the Arduino SD library (FatFs underneath),
 * whose SPI transactions coexist with the radio on a shared bus.
 */
class SpiBus : public ISdBus
{
  public:
    IStorage::Status mount(bool formatIfNeeded) override
    {
#ifdef SDCARD_INIT_SPI
        SDHandler.end();
#endif
#if defined(SPI_SCK) && defined(SPI_MISO) && defined(SPI_MOSI)
        SDHandler.begin(SPI_SCK, SPI_MISO, SPI_MOSI); // no-op when the bus is already running
#endif
        SD.end();
        if (SD.begin(SDCARD_CS, SDHandler, SD_SPI_FREQUENCY, VFS_PATH, MAX_FILES, formatIfNeeded)) {
            patientCard(drive());
            return IStorage::Status::Ok;
        }

        // SD.begin() fails alike for an empty slot and a card without filesystem
        uint8_t pdrv = sdcard_init(SDCARD_CS, &SDHandler, SD_SPI_FREQUENCY);
        if (pdrv == 0xFF)
            return IStorage::Status::CardError;
        bool present = !(disk_initialize(pdrv) & STA_NOINIT);
        sdcard_uninit(pdrv);
        return present ? IStorage::Status::NoFilesystem : IStorage::Status::SlotEmpty;
    }

    void unmount(void) override { SD.end(); }

    uint8_t drive(void) const override { return SD.*(&SdDrive::_pdrv); }

    StorageInfo::CardType cardType(void) override
    {
        switch (SD.cardType()) {
        case CARD_NONE:
            return StorageInfo::eNone;
        case CARD_MMC:
            return StorageInfo::eMMC;
        case CARD_SD:
            return StorageInfo::eSD;
        case CARD_SDHC:
            // SDXC carries the SDHC capability bit as well; only the size separates them
            return SD.cardSize() > 32ULL * 1024 * 1024 * 1024 ? StorageInfo::eSDXC : StorageInfo::eSDHC;
        default:
            return StorageInfo::eUnknown;
        }
    }

    fs::FS *arduinoFS(void) override { return &SD; }
};
} // namespace

std::unique_ptr<ISdBus> createSpiBus(void)
{
    return std::unique_ptr<ISdBus>(new SpiBus);
}

#endif
