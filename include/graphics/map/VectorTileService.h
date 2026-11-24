#pragma once
#include "graphics/map/TileService.h"
#include "storage/IStorage.h"
#include "util/VectorMap.h"
#include <memory>
#include <string>

/**
 * Tiles drawn on the spot from the vector maps on the storage (util/VectorMap.h) instead of read as pictures.
 * While a vector map is in use every tile comes from it; otherwise, and for tiles it has nothing for, the
 * fallback service is asked, which is the one for picture tiles.
 */
class VectorTileService : public ITileService
{
  public:
    // takes ownership of fallback
    explicit VectorTileService(IStorage &storage, ITileService *fallback = nullptr);
    virtual ~VectorTileService();
    bool load(const char *name, void *img) override;
    bool save(const char *name, void *img, size_t len) override;

    // the vector maps to draw from: all files *.jmv of that folder on the storage, each a state or region,
    // side by side; the folders "state" and "province" count as one. Empty: none, picture tiles again.
    // False: no vector map in there
    static bool use(const std::string &folder);
    static bool inUse(void);
    // what the map in use says about itself; valid while inUse()
    static uint8_t minZoom(void);
    static uint8_t maxZoom(void);
    static void center(double &latitude, double &longitude);
    // dark ground or light, and which layers; the tiles on screen have to be drawn anew afterwards
    static void setDark(bool dark);
    static VectorMap::Layers &layers(void);
    /**
     * How far it is from a place (degrees) to the nearest border of a country, by the maps in use: meters,
     * and the bearing to it. crossed: the way from the place asked about before to this one went across one.
     * False when no map in use covers the place: nothing is known then.
     */
    static bool border(double latitude, double longitude, float &meters, float &bearing, bool &crossed);
    // how long the last tile took, ms (reading and drawing)
    static uint32_t lastTileMs(void);

  private:
    ITileService *fallback;
};
