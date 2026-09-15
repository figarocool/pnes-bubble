//
// Vita "game bubble" generator implementation.
//

#include "bubble_maker.h"
#include "pemu.h"

#ifdef __VITA__
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "sha1.h"
#include "head_bin.h"
#endif

#include "ss_curl.h"
#include <png.h>
#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <vector>

static void blog(const char *fmt, ...) {
    FILE *f = fopen("ux0:data/bubble_debug.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

using namespace c2d;
using namespace pemu;

// tiny FNV-1a hash, used only to derive a stable 9-char titleid from the game name
static uint32_t fnv1a(const std::string &s) {
    uint32_t h = 2166136261u;
    for (char c: s) {
        h ^= (uint8_t) c;
        h *= 16777619u;
    }
    return h;
}

std::string BubbleMaker::makeTitleId(const std::string &name) {
    uint32_t h = fnv1a(name);
    char buf[10];
    // "NB" prefix + 7 hex digits = 9 chars total, matching vita titleid length
    snprintf(buf, sizeof(buf), "NB%07X", h & 0x0FFFFFFF);
    return std::string(buf);
}

// PSF (param.sfo) format: 20 byte header, then N x 16 byte index entries,
// then the key table (null terminated strings), then the data table.
// We only ever patch existing string fields in place, keeping every offset
// and every other field byte-identical to the real, working param.sfo this
// app was installed with - this avoids re-implementing sfo generation from
// scratch and the bugs that would come with it.
bool BubbleMaker::patchSfo(std::vector<char> &sfo, const std::string &titleId,
                            const std::string &title) {
    if (sfo.size() < 20) return false;

    auto readU32 = [&](size_t off) {
        uint32_t v;
        memcpy(&v, sfo.data() + off, 4);
        return v;
    };

    uint32_t keyTableStart = readU32(8);
    uint32_t dataTableStart = readU32(12);
    uint32_t count = readU32(16);

    for (uint32_t i = 0; i < count; i++) {
        size_t entryOff = 20 + i * 16;
        if (entryOff + 16 > sfo.size()) break;

        uint16_t keyOffset;
        uint16_t dataFmt;
        uint32_t dataLen, dataMaxLen, dataOffset;
        memcpy(&keyOffset, sfo.data() + entryOff, 2);
        memcpy(&dataFmt, sfo.data() + entryOff + 2, 2);
        memcpy(&dataLen, sfo.data() + entryOff + 4, 4);
        memcpy(&dataMaxLen, sfo.data() + entryOff + 8, 4);
        memcpy(&dataOffset, sfo.data() + entryOff + 12, 4);

        size_t keyStart = keyTableStart + keyOffset;
        if (keyStart >= sfo.size()) continue;
        std::string key(sfo.data() + keyStart);

        const std::string *newValue = nullptr;
        if (key == "TITLE_ID") newValue = &titleId;
        else if (key == "TITLE") newValue = &title;
        else if (key == "STITLE") newValue = &title;

        if (!newValue) continue;

        size_t dataStart = dataTableStart + dataOffset;
        if (dataStart + dataMaxLen > sfo.size()) continue;

        std::string v = *newValue;
        if (v.size() + 1 > dataMaxLen) v.resize(dataMaxLen - 1);

        // zero the whole fixed-size slot, then write the new (shorter or
        // equal) null terminated string into it - keeps dataMaxLen untouched
        memset(sfo.data() + dataStart, 0, dataMaxLen);
        memcpy(sfo.data() + dataStart, v.c_str(), v.size() + 1);

        uint32_t newLen = (uint32_t) v.size() + 1;
        memcpy(sfo.data() + entryOff + 4, &newLen, 4);
    }

    return true;
}

static bool readFile(c2d::Io *io, const std::string &path, std::vector<char> &out) {
    size_t size = io->getSize(path);
    if (size == 0) return false;
    out.resize(size);
    return io->read(path, out.data(), size) == size;
}

// io->copy() relies on stat() to tell files from directories, which is not
// reliable on the read-only "app0:" package filesystem - so copy plain files
// ourselves via a full read + full write instead.
static bool copyFileManual(c2d::Io *io, const std::string &src, const std::string &dst) {
    std::vector<char> buf;
    if (!readFile(io, src, buf)) return false;
    return io->write(dst, buf.data(), buf.size());
}

// looks up the rom's real no-intro name by its CRC32, independent of the rom
// file's own name on disk - so "bayoubilly.nes" resolves to the same, correct
// "Adventures of Bayou Billy, The (USA)" title that a properly-named rom
// would. Uses the libretro-database NES dat bundled in the app's own
// package (same source libretro-thumbnails itself keys box art names off).
static bool lookupNoIntroName(c2d::Io *io, const std::string &romPath,
                               const std::string &romFs, std::string &outName) {
    std::vector<char> rom;
    if (!readFile(io, romPath, rom)) return false;
    uLong crc = crc32(0L, nullptr, 0);
    crc = crc32(crc, (const Bytef *) rom.data(), (uInt) rom.size());

    char needle[16];
    snprintf(needle, sizeof(needle), "crc %08lx", (unsigned long) crc);

    std::vector<char> dat;
    if (!readFile(io, romFs + "NoIntro-NES.dat", dat)) {
        blog("lookupNoIntroName: failed to read NoIntro-NES.dat");
        return false;
    }

    std::string hay(dat.data(), dat.size());
    size_t crcPos = hay.find(needle);
    if (crcPos == std::string::npos) {
        blog("lookupNoIntroName: crc %08lx not found in database", (unsigned long) crc);
        return false;
    }

    size_t nameKeyPos = hay.rfind("name \"", crcPos);
    if (nameKeyPos == std::string::npos) return false;
    size_t nameStart = nameKeyPos + 6;
    size_t nameEnd = hay.find('"', nameStart);
    if (nameEnd == std::string::npos) return false;

    outName = hay.substr(nameStart, nameEnd - nameStart);
    blog("lookupNoIntroName: crc=%08lx -> \"%s\"", (unsigned long) crc, outName.c_str());
    return true;
}

// recursively copies every file pnes ships in its own read-only package
// (skins, fonts, database...) into the new bubble's package, skipping the
// bits we regenerate/replace ourselves (sce_sys, and the "bubble/" folder
// where the target rom gets bundled instead).
static void copyDirRecursive(c2d::Io *io, const std::string &srcDir, const std::string &dstDir) {
    io->create(dstDir);
    auto entries = io->getDirList(srcDir, false, false);
    for (const auto &entry: entries) {
        if (entry.name == "." || entry.name == "..") continue;
        if (entry.name == "sce_sys" || entry.name == "bubble") continue;

        std::string srcPath = srcDir + entry.name;
        std::string dstPath = dstDir + entry.name;
        if (entry.type == c2d::Io::Type::Directory) {
            copyDirRecursive(io, srcPath + "/", dstPath + "/");
        } else {
            copyFileManual(io, srcPath, dstPath);
        }
    }
}

#ifdef __VITA__
// reads a string field out of an already-parsed param.sfo buffer (same
// layout patchSfo() understands)
static std::string sfoGetString(const std::vector<char> &sfo, const char *wantKey) {
    if (sfo.size() < 20) return "";
    uint32_t keyTableStart, dataTableStart, count;
    memcpy(&keyTableStart, sfo.data() + 8, 4);
    memcpy(&dataTableStart, sfo.data() + 12, 4);
    memcpy(&count, sfo.data() + 16, 4);

    for (uint32_t i = 0; i < count; i++) {
        size_t entryOff = 20 + i * 16;
        if (entryOff + 16 > sfo.size()) break;
        uint16_t keyOffset;
        uint32_t dataOffset;
        memcpy(&keyOffset, sfo.data() + entryOff, 2);
        memcpy(&dataOffset, sfo.data() + entryOff + 12, 4);

        size_t keyStart = keyTableStart + keyOffset;
        if (keyStart >= sfo.size()) continue;
        std::string key(sfo.data() + keyStart);
        if (key != wantKey) continue;

        size_t dataStart = dataTableStart + dataOffset;
        if (dataStart >= sfo.size()) continue;
        return std::string(sfo.data() + dataStart);
    }
    return "";
}

#define ntohl_local __builtin_bswap32

// port of VitaDeploy's promote.c fpkg_hmac() - a fixed, non-standard
// checksum scheme the promoter service expects inside sce_sys/package/head.bin,
// not a real SHA1-HMAC. Kept byte-for-byte identical to the reference.
static void fpkg_hmac(const uint8_t *data, unsigned int len, uint8_t hmac[16]) {
    SHA1_CTX ctx;
    uint8_t sha1[20];
    uint8_t buf[64];

    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, sha1);

    memset(buf, 0, 64);
    memcpy(&buf[0], &sha1[4], 8);
    memcpy(&buf[8], &sha1[4], 8);
    memcpy(&buf[16], &sha1[12], 4);
    buf[20] = sha1[16];
    buf[21] = sha1[1];
    buf[22] = sha1[2];
    buf[23] = sha1[3];
    memcpy(&buf[24], &buf[16], 8);

    sha1_init(&ctx);
    sha1_update(&ctx, buf, 64);
    sha1_final(&ctx, sha1);
    memcpy(hmac, sha1, 16);
}

// scePromoterUtilityPromotePkgWithRif requires a sce_sys/package/head.bin
// file (a minimal fake pkg header) or it fails with 0x8010111C. Port of
// VitaDeploy's makeHead(), adapted to write through our Io abstraction.
static bool writeHeadBin(c2d::Io *io, const std::string &stagingDir,
                          const std::vector<char> &sfo) {
    std::string titleId = sfoGetString(sfo, "TITLE_ID");
    std::string contentId = sfoGetString(sfo, "CONTENT_ID");

    std::vector<uint8_t> head(tpl_head_bin, tpl_head_bin + tpl_head_bin_len);

    char fullTitleId[48];
    snprintf(fullTitleId, sizeof(fullTitleId), "EP9000-%s_00-0000000000000000", titleId.c_str());
    const std::string &contentIdOrFallback = !contentId.empty() ? contentId : std::string(fullTitleId);
    memset(&head[0x30], 0, 48);
    memcpy(&head[0x30], contentIdOrFallback.c_str(),
           std::min<size_t>(48, contentIdOrFallback.size()));

    uint8_t hmac[16];
    uint32_t len, off, out;

    memcpy(&len, &head[0xD0], 4);
    len = ntohl_local(len);
    fpkg_hmac(&head[0], len, hmac);
    memcpy(&head[len], hmac, 16);

    memcpy(&off, &head[0x8], 4);
    off = ntohl_local(off);
    memcpy(&len, &head[0x10], 4);
    len = ntohl_local(len);
    memcpy(&out, &head[0xD4], 4);
    out = ntohl_local(out);
    fpkg_hmac(&head[off], len - 64, hmac);
    memcpy(&head[out], hmac, 16);

    memcpy(&len, &head[0xE8], 4);
    len = ntohl_local(len);
    fpkg_hmac(&head[0], len, hmac);
    memcpy(&head[len], hmac, 16);

    io->create(stagingDir + "sce_sys/package/");
    return io->write(stagingDir + "sce_sys/package/head.bin",
                      (const char *) head.data(), head.size());
}

// libretro-thumbnails names images after the exact no-intro game title, with
// a fixed set of characters replaced by "_" - see its own README. This is
// separate from URL-escaping (spaces, commas, parens are fine once escaped).
static std::string sanitizeThumbnailName(const std::string &title) {
    static const std::string bad = "&*/:`<>?\\|";
    std::string out = title;
    for (char &c: out) {
        if (bad.find(c) != std::string::npos) c = '_';
    }
    return out;
}

struct BoxArt {
    std::vector<uint8_t> pixels;
    int width = 0, height = 0;
};

// downloads the game's NES box art from libretro-thumbnails once, decoded
// into memory - callers derive as many differently-sized PNGs from it as
// they need (icon0.png, LiveArea bg.png, startup.png) without re-downloading.
static bool downloadBoxArt(c2d::Io *io, const std::string &title, BoxArt &out) {
    ss_api::Curl curl;
    std::string url = "https://raw.githubusercontent.com/libretro-thumbnails/"
                       "Nintendo_-_Nintendo_Entertainment_System/master/Named_Boxarts/"
                       + curl.escape(sanitizeThumbnailName(title)) + ".png";

    std::string tmpPath = "ux0:data/pnes/tmp_boxart.png";
    long httpCode = 0;
    int ret = curl.getData(url, tmpPath, 10, &httpCode);
    blog("downloadBoxArt: url=%s ret=%d http_code=%ld", url.c_str(), ret, httpCode);

    // ss_api::Curl::getData() remaps a real HTTP 200 response down to
    // http_code=0 (its own "success" convention) - any other value here,
    // curl-error or real HTTP status (404, ...), means failure.
    if (ret != 0 || httpCode != 0 || !io->exist(tmpPath) || io->getSize(tmpPath) == 0) {
        io->removeFile(tmpPath);
        return false;
    }

    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_file(&image, tmpPath.c_str())) {
        blog("downloadBoxArt: png_image_begin_read_from_file failed: %s", image.message);
        io->removeFile(tmpPath);
        return false;
    }

    image.format = PNG_FORMAT_RGBA;
    out.pixels.resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, out.pixels.data(), 0, nullptr)) {
        blog("downloadBoxArt: png_image_finish_read failed: %s", image.message);
        png_image_free(&image);
        io->removeFile(tmpPath);
        return false;
    }
    out.width = (int) image.width;
    out.height = (int) image.height;
    png_image_free(&image);
    io->removeFile(tmpPath);

    blog("downloadBoxArt: OK, %dx%d", out.width, out.height);
    return true;
}

// nearest-neighbor stretch of the downloaded box art to (dstW x dstH),
// dropping alpha (box art has none worth keeping) - shared by all 3 sizes.
static std::vector<uint8_t> resizeToRgb(const BoxArt &src, int dstW, int dstH) {
    std::vector<uint8_t> dstPixels((size_t) dstW * dstH * 3);
    for (int y = 0; y < dstH; y++) {
        int sy = y * src.height / dstH;
        for (int x = 0; x < dstW; x++) {
            int sx = x * src.width / dstW;
            const uint8_t *s = &src.pixels[(sy * src.width + sx) * 4];
            uint8_t *d = &dstPixels[(y * dstW + x) * 3];
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    return dstPixels;
}

// Sony's LiveArea image validator rejects plain truecolor PNGs with
// 0x8010113D ("livearea images not processed through pngquant") - it wants
// palette (indexed-color) PNGs. We don't have pngquant's real quantizer
// available on-device, so we quantize onto a fixed 6x6x6 (216 color) "web
// safe" palette instead: simple, always <= 256 colors, good enough for a
// small home-screen image.
static bool writeIndexedPng(const std::vector<uint8_t> &rgb, int w, int h, const std::string &dstPath) {
    png_color palette[216];
    for (int i = 0; i < 216; i++) {
        palette[i].red = (png_byte) ((i / 36) * 51);
        palette[i].green = (png_byte) (((i / 6) % 6) * 51);
        palette[i].blue = (png_byte) ((i % 6) * 51);
    }

    std::vector<uint8_t> indices((size_t) w * h);
    for (size_t i = 0; i < indices.size(); i++) {
        int r = (rgb[i * 3 + 0] + 25) / 51;
        int g = (rgb[i * 3 + 1] + 25) / 51;
        int b = (rgb[i * 3 + 2] + 25) / 51;
        if (r > 5) r = 5;
        if (g > 5) g = 5;
        if (b > 5) b = 5;
        indices[i] = (uint8_t) (r * 36 + g * 6 + b);
    }

    FILE *fp = fopen(dstPath.c_str(), "wb");
    if (!fp) {
        blog("writeIndexedPng: fopen failed for %s", dstPath.c_str());
        return false;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info) {
        blog("writeIndexedPng: png_create_*_struct failed");
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        blog("writeIndexedPng: libpng error while writing %s", dstPath.c_str());
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_PALETTE,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_PLTE(png, info, palette, 216);
    png_write_info(png, info);

    std::vector<png_bytep> rows(h);
    for (int y = 0; y < h; y++) rows[y] = &indices[(size_t) y * w];
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);

    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return true;
}

// nearest-neighbor stretch of the downloaded box art to (dstW x dstH) and
// save as an indexed PNG (see writeIndexedPng) - used for icon0.png
// (128x128), LiveArea bg.png (840x500) and startup.png (320x240), which all
// have different aspect ratios than typical box art, so this stretches
// rather than crops.
static bool saveResizedPng(const BoxArt &src, int dstW, int dstH, const std::string &dstPath) {
    std::vector<uint8_t> rgb = resizeToRgb(src, dstW, dstH);
    if (!writeIndexedPng(rgb, dstW, dstH, dstPath)) {
        blog("saveResizedPng: write failed for %s", dstPath.c_str());
        return false;
    }
    return true;
}
#endif

std::string BubbleMaker::createBubble(UiMain *ui, const ss_api::Game &game) {
#ifndef __VITA__
    (void) ui;
    (void) game;
    return "not supported on this platform";
#else
    c2d::Io *io = ui->getIo();
    const std::string romFs = io->getRomFsPath(); // "app0:/"
    const std::string srcRom = game.romsPath + game.path;

    if (!io->exist(srcRom)) {
        return "rom file not found";
    }

    // display title: strip the file extension if the scanner left it in game.name
    std::string title = game.name;
    size_t dot = title.find_last_of('.');
    if (dot != std::string::npos) title = title.substr(0, dot);

    // prefer the real no-intro name looked up by the rom's CRC32, so a
    // renamed/mislabeled rom file still gets the correct title and box art
    std::string dbName;
    if (lookupNoIntroName(io, srcRom, romFs, dbName)) {
        title = dbName;
    }

    blog("=== createBubble: name=%s title=%s romFs=%s srcRom=%s ===",
         game.name.c_str(), title.c_str(), romFs.c_str(), srcRom.c_str());
    blog("exist(srcRom)=%d", (int) io->exist(srcRom));

    // based on the resolved title (not the raw filename) so two differently
    // named copies of the same rom map to the same bubble/titleid
    std::string titleId = makeTitleId(title);
    // NOTE: io->getDataPath() is already scoped to "ux0:/data/pnes/" on vita
    // (see PNESIo::getDataPath()) - do not prepend "pnes/" again here.
    std::string stagingDir = io->getDataPath() + "bubble_staging/" + titleId + "/";
    blog("getDataPath()=%s titleId=%s stagingDir=%s",
         io->getDataPath().c_str(), titleId.c_str(), stagingDir.c_str());

    io->create(io->getDataPath() + "bubble_staging/");
    io->removeDir(stagingDir);
    io->create(stagingDir);
    io->create(stagingDir + "sce_sys/");
    io->create(stagingDir + "bubble/");
    blog("exist(stagingDir)=%d", (int) io->exist(stagingDir));

    // copy every resource pnes ships with itself (skins, fonts, database...)
    // so the cloned bubble is fully self-contained, not just eboot+icon+rom
    blog("copying full app0: package contents...");
    copyDirRecursive(io, romFs, stagingDir);
    blog("exist(eboot dst)=%d size(eboot dst)=%u",
         (int) io->exist(stagingDir + "eboot.bin"),
         (unsigned) io->getSize(stagingDir + "eboot.bin"));

    BoxArt boxArt;
    bool haveBoxArt = downloadBoxArt(io, title, boxArt);

    std::string iconDst = stagingDir + "sce_sys/icon0.png";
    if (haveBoxArt && saveResizedPng(boxArt, 128, 128, iconDst)) {
        blog("using downloaded box art as icon0.png");
    } else {
        std::string iconSrc = romFs + "sce_sys/icon0.png";
        blog("icon: box art unavailable, falling back to pnes icon: path=%s exist=%d size=%u",
             iconSrc.c_str(), (int) io->exist(iconSrc), (unsigned) io->getSize(iconSrc));
        if (!copyFileManual(io, iconSrc, iconDst)) {
            blog("FAILED copying fallback icon0.png");
            return "failed to copy icon0.png";
        }
        blog("fallback icon0.png copied OK");
    }

    // LiveArea background + startup splash - copyDirRecursive() above already
    // copied pnes's own generic ones as a baseline, overwrite them with the
    // game's box art when we have it (bg.png 840x500, startup.png 320x240)
    io->create(stagingDir + "sce_sys/livearea/");
    io->create(stagingDir + "sce_sys/livearea/contents/");
    copyFileManual(io, romFs + "sce_sys/livearea/contents/template.xml",
                   stagingDir + "sce_sys/livearea/contents/template.xml");

    if (haveBoxArt) {
        std::string bgDst = stagingDir + "sce_sys/livearea/contents/bg.png";
        std::string startupDst = stagingDir + "sce_sys/livearea/contents/startup.png";
        bool bgOk = saveResizedPng(boxArt, 840, 500, bgDst);
        bool startupOk = saveResizedPng(boxArt, 320, 240, startupDst);
        blog("LiveArea assets from box art: bg=%d startup=%d", (int) bgOk, (int) startupOk);
    } else {
        copyFileManual(io, romFs + "sce_sys/livearea/contents/bg.png",
                       stagingDir + "sce_sys/livearea/contents/bg.png");
        copyFileManual(io, romFs + "sce_sys/livearea/contents/startup.png",
                       stagingDir + "sce_sys/livearea/contents/startup.png");
    }

    blog("rom: path=%s exist=%d size=%u", srcRom.c_str(),
         (int) io->exist(srcRom), (unsigned) io->getSize(srcRom));

    // NOTE: must match the "bubble/" path main.cpp checks at startup via
    // io->getRomFsPath() + "bubble/" (not "data/bubble/")
    if (!copyFileManual(io, srcRom, stagingDir + "bubble/" + game.path)) {
        blog("FAILED copying rom file");
        return "failed to copy rom file";
    }
    blog("rom file copied OK");

    std::string sfoSrc = romFs + "sce_sys/param.sfo";
    blog("sfo: path=%s exist=%d size=%u", sfoSrc.c_str(),
         (int) io->exist(sfoSrc), (unsigned) io->getSize(sfoSrc));

    std::vector<char> sfo;
    if (!readFile(io, sfoSrc, sfo)) {
        blog("FAILED reading param.sfo");
        return "failed to read param.sfo";
    }
    blog("param.sfo read OK, size=%u", (unsigned) sfo.size());

    if (!patchSfo(sfo, titleId, title)) {
        blog("FAILED patching param.sfo");
        return "failed to patch param.sfo";
    }
    blog("param.sfo patched OK");

    if (!io->write(stagingDir + "sce_sys/param.sfo", sfo.data(), sfo.size())) {
        blog("FAILED writing param.sfo");
        return "failed to write param.sfo";
    }
    blog("param.sfo written OK");

    if (!writeHeadBin(io, stagingDir, sfo)) {
        blog("FAILED writing head.bin");
        return "failed to write head.bin";
    }
    blog("head.bin written OK, loading PAF + PromoterUtil sysmodules...");

    // ScePromoterUtil depends on PAF being loaded first - without this, module
    // manager fails to resolve its imports (SCE_KERNEL_ERROR_MODULEMGR_NO_LIB).
    // Exact args taken from lpp-vita's loadPromoter(), which is the reference
    // implementation used by Adrenaline Bubble Manager for the same purpose.
    uint32_t pafOptBuf[4] = {0};
    SceSysmoduleOpt *pafOpt = (SceSysmoduleOpt *) pafOptBuf;
    uint32_t pafArgp[5] = {0x400000, 0xEA60, 0x40000, 0, 0};
    int pafRet = sceSysmoduleLoadModuleInternalWithArg(
            SCE_SYSMODULE_INTERNAL_PAF, sizeof(pafArgp), pafArgp, pafOpt);
    blog("sceSysmoduleLoadModuleInternalWithArg(PAF) returned 0x%08X", (unsigned) pafRet);

    int loadRet = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    blog("sceSysmoduleLoadModuleInternal returned 0x%08X", (unsigned) loadRet);

    int initRet = scePromoterUtilityInit();
    blog("scePromoterUtilityInit returned 0x%08X", (unsigned) initRet);

    int ret = scePromoterUtilityPromotePkgWithRif(stagingDir.c_str(), 1);
    blog("scePromoterUtilityPromotePkgWithRif returned 0x%08X", (unsigned) ret);

    scePromoterUtilityExit();
    sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);

    SceSysmoduleOpt pafUnloadOpt = {0, nullptr, {0, 0}};
    sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, nullptr, &pafUnloadOpt);

    io->removeDir(stagingDir);

    if (ret < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "install failed (0x%08X)", (unsigned) ret);
        return buf;
    }

    return "";
#endif
}
