/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
 * Copyright 2018 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarth/MBTiles>
#include <osgEarth/Registry>
#include <osgEarth/FileUtils>
#include <osgEarth/XmlUtils>
#include <osgEarth/ImageToHeightFieldConverter>
#include <osgDB/FileUtils>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <sqlite3.h>


using namespace osgEarth;
using namespace osgEarth::MBTiles;

#undef LC
#define LC "[MBTiles] "

//......................................................................

namespace
{
    osgDB::ReaderWriter* getReaderWriter(const std::string& format)
    {
        osgDB::ReaderWriter* rw = 0L;

        // Get a ReaderWriter for the tile format. Try both mime-type and extension.
        rw = osgDB::Registry::instance()->getReaderWriterForMimeType( format );
        if ( rw == 0L )
        {
            rw = osgDB::Registry::instance()->getReaderWriterForExtension( format );
        }
        return rw;
    }
}
//........................................................................

REGISTER_OSGEARTH_LAYER(mbtilesimage, MBTilesImageLayer);

OE_LAYER_PROPERTY_IMPL(MBTilesImageLayer, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(MBTilesImageLayer, std::string, Format, format);
OE_LAYER_PROPERTY_IMPL(MBTilesImageLayer, bool, Compress, compress);
OE_LAYER_PROPERTY_IMPL(MBTilesImageLayer, bool, ComputeLevels, computeLevels);

void
MBTilesImageLayer::init()
{
    ImageLayer::init();
    setTileSourceExpected(false);
    _database = 0L;
    _minLevel = 0u;
    _maxLevel = 20u;
    _forceRGB = false;
}

const Status&
MBTilesImageLayer::open()
{
    if (ImageLayer::open().isOK())
    {
        bool readWrite = false; // TODO // options().openForWriting() == true;

        std::string fullFilename = options().url()->full();
        if (!osgDB::fileExists(fullFilename))
        {
            fullFilename = osgDB::findDataFile(fullFilename, getReadOptions());
            if (fullFilename.empty())
                fullFilename = options().url()->full();
        }

        bool isNewDatabase = readWrite && !osgDB::fileExists(fullFilename);

        if (isNewDatabase)
        {
            // For a NEW database, the profile MUST be set prior to initialization.
            if (getProfile() == 0L)
            {
                return setStatus(Status::ConfigurationError, 
                    "Cannot create database; required Profile is missing");
            }

            // For a NEW database the format is required.
            if (options().format().isSet())
            {
                _tileFormat = options().format().value();
                _rw = getReaderWriter(_tileFormat);
                if (!_rw.valid())
                {
                    return setStatus(Status::ServiceUnavailable, 
                        "No plugin to load format \"" + _tileFormat + "\"");
                }
            }
            else
            {
                return setStatus(Status::ConfigurationError,
                    "Cannot create database; required format is missing");
            }

            OE_INFO << LC << "Database does not exist; attempting to create it." << std::endl;
        }

        // Try to open (or create) the database. We use SQLITE_OPEN_NOMUTEX to do
        // our own mutexing.
        int flags = readWrite
            ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX)
            : (SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);

        sqlite3* database = (sqlite3*)_database;
        sqlite3** dbptr = (sqlite3**)&_database;
        int rc = sqlite3_open_v2(fullFilename.c_str(), dbptr, flags, 0L);
        if (rc != 0)
        {
            return setStatus(Status::ResourceUnavailable, Stringify()
                << "Database \"" << fullFilename << "\": " << sqlite3_errmsg(database));
        }

        // New database setup:
        if (isNewDatabase)
        {
            // create necessary db tables:
            createTables();

            // write profile to metadata:
            std::string profileJSON = getProfile()->toProfileOptions().getConfig().toJSON(false);
            putMetaData("profile", profileJSON);

            // write format to metadata:
            putMetaData("format", _tileFormat);

            // compression?
            if (options().compress().isSetTo(true))
            {
                _compressor = osgDB::Registry::instance()->getObjectWrapperManager()->findCompressor("zlib");
                if (_compressor.valid())
                {
                    putMetaData("compression", "zlib");
                    OE_INFO << LC << "Data will be compressed (zlib)" << std::endl;
                }
            }

            // If we have some data extents at this point, write the bounds to the metadata.
            if (getDataExtents().size() > 0)
            {
                // Get the union of all the extents
                GeoExtent e(getDataExtents()[0]);
                for (unsigned int i = 1; i < getDataExtents().size(); i++)
                {
                    e.expandToInclude(getDataExtents()[i]);
                }

                // Convert the bounds to wgs84
                GeoExtent bounds = e.transform(osgEarth::SpatialReference::get("wgs84"));
                std::stringstream boundsStr;
                boundsStr << bounds.xMin() << "," << bounds.yMin() << "," << bounds.xMax() << "," << bounds.yMax();
                putMetaData("bounds", boundsStr.str());
            }
        }

        // If the database pre-existed, read in the information from the metadata.
        else // !isNewDatabase
        {
            if (options().computeLevels() == true)
            {
                computeLevels();
            }

            std::string profileStr;
            getMetaData("profile", profileStr);

            // The data format (e.g., png, jpg, etc.). Any format passed in
            // in the options is superseded by the one in the database metadata.
            std::string metaDataFormat;
            getMetaData("format", metaDataFormat);
            if (!metaDataFormat.empty())
                _tileFormat = metaDataFormat;

            // Try to get it from the options.
            if (_tileFormat.empty())
            {
                if (options().format().isSet())
                {
                    _tileFormat = options().format().value();
                }
            }

            // By this point, we require a valid tile format.
            if (_tileFormat.empty())
            {
                return setStatus(Status::ConfigurationError, "Required format not in metadata, nor specified in the options.");
            }

            // check for compression.
            std::string compression;
            getMetaData("compression", compression);
            if (!compression.empty())
            {
                _compressor = osgDB::Registry::instance()->getObjectWrapperManager()->findCompressor(compression);
                if (!_compressor.valid())
                    return setStatus(Status::ServiceUnavailable, "Cannot find compressor \"" + compression + "\"");
                else
                    OE_INFO << LC << "Data is compressed (" << compression << ")" << std::endl;
            }

            // Set the profile
            const Profile* profile = getProfile();
            if (!profile)
            {
                if (!profileStr.empty())
                {
                    // try to parse it as a JSON config
                    Config pconf;
                    pconf.fromJSON(profileStr);
                    profile = Profile::create(ProfileOptions(pconf));

                    // if that didn't work, try parsing it directly
                    if (!profile)
                    {
                        profile = Profile::create(profileStr);
                    }
                }

                if (!profile)
                {
                    OE_WARN << LC << "Profile \"" << profileStr << "\" not recognized; defaulting to spherical-mercator\n";
                    profile = Profile::create("spherical-mercator");
                }

                setProfile(profile);
                OE_INFO << LC << "Profile = " << profile->toString() << std::endl;
            }

            // Check for bounds and populate DataExtents.
            std::string boundsStr;
            if (getMetaData("bounds", boundsStr))
            {
                std::vector<std::string> tokens;
                StringTokenizer(",").tokenize(boundsStr, tokens);
                if (tokens.size() == 4)
                {
                    double minLon = osgEarth::as<double>(tokens[0], 0.0);
                    double minLat = osgEarth::as<double>(tokens[1], 0.0);
                    double maxLon = osgEarth::as<double>(tokens[2], 0.0);
                    double maxLat = osgEarth::as<double>(tokens[3], 0.0);

                    GeoExtent extent(osgEarth::SpatialReference::get("wgs84"), minLon, minLat, maxLon, maxLat);
                    if (extent.isValid())
                    {
                        // Using 0 for the minLevel is not technically correct, but we use it instead of the proper minLevel to force osgEarth to subdivide
                        // since we don't really handle DataExtents with minLevels > 0 just yet.
                        dataExtents().push_back(DataExtent(extent, 0, _maxLevel));
                        OE_INFO << LC << "Bounds = " << extent.toString() << std::endl;
                    }
                    else
                    {
                        OE_WARN << LC << "MBTiles has invalid bounds " << extent.toString() << std::endl;
                    }
                }
            }
            else
            {
                // Using 0 for the minLevel is not technically correct, but we use it instead of the proper minLevel to force osgEarth to subdivide
                // since we don't really handle DataExtents with minLevels > 0 just yet.
                this->dataExtents().push_back(DataExtent(getProfile()->getExtent(), 0, _maxLevel));
            }
        }

        // do we require RGB? for jpeg?
        _forceRGB =
            osgEarth::endsWith(_tileFormat, "jpg", false) ||
            osgEarth::endsWith(_tileFormat, "jpeg", false);

        // make an empty image.
        int size = 256;
        _emptyImage = new osg::Image();
        _emptyImage->allocateImage(size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        unsigned char *data = _emptyImage->data(0, 0);
        memset(data, 0, 4 * size * size);
    }
    return getStatus();
}

GeoImage
MBTilesImageLayer::createImageImplementation(const TileKey& key, ProgressCallback* progress) const
{
    Threading::ScopedMutexLock exclusiveLock(_mutex);

    int z = key.getLevelOfDetail();
    int x = key.getTileX();
    int y = key.getTileY();

    if (z < (int)_minLevel)
    {
        return GeoImage(_emptyImage.get(), key.getExtent());
    }

    if (z > (int)_maxLevel)
    {
        //If we're at the max level, just return NULL
        return GeoImage::INVALID;
    }

    unsigned int numRows, numCols;
    key.getProfile()->getNumTiles(key.getLevelOfDetail(), numCols, numRows);
    y  = numRows - y - 1;

    sqlite3* database = (sqlite3*)_database;

    //Get the image
    sqlite3_stmt* select = NULL;
    std::string query = "SELECT tile_data from tiles where zoom_level = ? AND tile_column = ? AND tile_row = ?";
    int rc = sqlite3_prepare_v2( database, query.c_str(), -1, &select, 0L );
    if ( rc != SQLITE_OK )
    {
        OE_WARN << LC << "Failed to prepare SQL: " << query << "; " << sqlite3_errmsg(database) << std::endl;
        return GeoImage::INVALID;
    }

    bool valid = true;

    sqlite3_bind_int( select, 1, z );
    sqlite3_bind_int( select, 2, x );
    sqlite3_bind_int( select, 3, y );

    osg::Image* result = NULL;
    rc = sqlite3_step( select );
    if ( rc == SQLITE_ROW)
    {
        // the pointer returned from _blob gets freed internally by sqlite, supposedly
        const char* data = (const char*)sqlite3_column_blob( select, 0 );
        int dataLen = sqlite3_column_bytes( select, 0 );

        std::string dataBuffer( data, dataLen );

        // decompress if necessary:
        if ( _compressor.valid() )
        {
            std::istringstream inputStream(dataBuffer);
            std::string value;
            if ( !_compressor->decompress(inputStream, value) )
            {
                if ( options().url().isSet() )
                    OE_WARN << LC << "Decompression failed: " << options().url()->base() << std::endl;
                else
                    OE_WARN << LC << "Decompression failed" << std::endl;
                valid = false;
            }
            else
            {
                dataBuffer = value;
            }
        }

        // decode the raw image data:
        if ( valid )
        {
            std::istringstream inputStream(dataBuffer);
            result = ImageUtils::readStream(inputStream, _dbOptions.get());
        }
    }
    else
    {
        OE_DEBUG << LC << "SQL QUERY failed for " << query << ": " << std::endl;
        valid = false;
    }

    sqlite3_finalize( select );

    if (result)
    {
        if (options().coverage() == true)
        {
            result->setInternalTextureFormat(GL_R16F);
            ImageUtils::markAsUnNormalized(result, true);
        }
    }

    return GeoImage(result, key.getExtent());
}
bool
MBTilesImageLayer::getMetaData(const std::string& key, std::string& value)
{
    Threading::ScopedMutexLock exclusiveLock(_mutex);
    
    sqlite3* database = (sqlite3*)_database;

    //get the metadata
    sqlite3_stmt* select = NULL;
    std::string query = "SELECT value from metadata where name = ?";
    int rc = sqlite3_prepare_v2( database, query.c_str(), -1, &select, 0L );
    if ( rc != SQLITE_OK )
    {
        OE_WARN << LC << "Failed to prepare SQL: " << query << "; " << sqlite3_errmsg(database) << std::endl;
        return false;
    }


    bool valid = true;
    std::string keyStr = std::string( key );
    rc = sqlite3_bind_text( select, 1, keyStr.c_str(), keyStr.length(), SQLITE_STATIC );
    if (rc != SQLITE_OK )
    {
        OE_WARN << LC << "Failed to bind text: " << query << "; " << sqlite3_errmsg(database) << std::endl;
        return false;
    }

    rc = sqlite3_step( select );
    if ( rc == SQLITE_ROW)
    {
        value = (char*)sqlite3_column_text( select, 0 );
    }
    else
    {
        OE_DEBUG << LC << "SQL QUERY failed for " << query << ": " << std::endl;
        valid = false;
    }

    sqlite3_finalize( select );
    return valid;
}

bool
MBTilesImageLayer::putMetaData(const std::string& key, const std::string& value)
{
    Threading::ScopedMutexLock exclusiveLock(_mutex);

    sqlite3* database = (sqlite3*)_database;

    // prep the insert statement.
    sqlite3_stmt* insert = 0L;
    std::string query = Stringify() << "INSERT OR REPLACE INTO metadata (name,value) VALUES (?,?)";
    if ( SQLITE_OK != sqlite3_prepare_v2(database, query.c_str(), -1, &insert, 0L) )
    {
        OE_WARN << LC << "Failed to prepare SQL: " << query << "; " << sqlite3_errmsg(database) << std::endl;
        return false;
    }

    // bind the values:
    if( SQLITE_OK != sqlite3_bind_text(insert, 1, key.c_str(), key.length(), SQLITE_STATIC) )
    {
        OE_WARN << LC << "Failed to bind text: " << query << "; " << sqlite3_errmsg(database) << std::endl;
        return false;
    }
    if ( SQLITE_OK != sqlite3_bind_text(insert, 2, value.c_str(), value.length(), SQLITE_STATIC) )
    {
        OE_WARN << LC << "Failed to bind text: " << query << "; " << sqlite3_errmsg(database) << std::endl;
        return false;
    }

    // execute the sql. no idea what a good return value should be :/
    sqlite3_step( insert );
    sqlite3_finalize( insert );
    return true;
}

void
MBTilesImageLayer::computeLevels()
{
    Threading::ScopedMutexLock exclusiveLock(_mutex);

    sqlite3* database = (sqlite3*)_database;
    osg::Timer_t startTime = osg::Timer::instance()->tick();
    sqlite3_stmt* select = NULL;
    std::string query = "SELECT min(zoom_level), max(zoom_level) from tiles";
    int rc = sqlite3_prepare_v2( database, query.c_str(), -1, &select, 0L );
    if ( rc != SQLITE_OK )
    {
        OE_WARN << LC << "Failed to prepare SQL: " << query << "; " << sqlite3_errmsg(database) << std::endl;
    }

    rc = sqlite3_step( select );
    if ( rc == SQLITE_ROW)
    {
        _minLevel = sqlite3_column_int( select, 0 );
        _maxLevel = sqlite3_column_int( select, 1 );
        OE_DEBUG << LC << "Min=" << _minLevel << " Max=" << _maxLevel << std::endl;
    }
    else
    {
        OE_DEBUG << LC << "SQL QUERY failed for " << query << ": " << std::endl;
    }
    sqlite3_finalize( select );
    osg::Timer_t endTime = osg::Timer::instance()->tick();
    OE_DEBUG << LC << "Computing levels took " << osg::Timer::instance()->delta_s(startTime, endTime ) << " s" << std::endl;
}

bool
MBTilesImageLayer::createTables()
{
    Threading::ScopedMutexLock exclusiveLock(_mutex);

    // https://github.com/mapbox/mbtiles-spec/blob/master/1.2/spec.md
    
    sqlite3* database = (sqlite3*)_database;

    std::string query =
        "CREATE TABLE IF NOT EXISTS metadata ("
        " name  text,"
        " value text)";

    if (SQLITE_OK != sqlite3_exec(database, query.c_str(), 0L, 0L, 0L))
    {
        OE_WARN << LC << "Failed to create table [metadata]" << std::endl;
        return false;
    }

    query =
        "CREATE TABLE IF NOT EXISTS tiles ("
        " zoom_level integer,"
        " tile_column integer,"
        " tile_row integer,"
        " tile_data blob)";

    char* errorMsg = 0L;

    if (SQLITE_OK != sqlite3_exec(database, query.c_str(), 0L, 0L, &errorMsg))
    {
        OE_WARN << LC << "Failed to create table [tiles]: " << errorMsg << std::endl;
        sqlite3_free( errorMsg );
        return false;
    }

    // create an index
    query =
        "CREATE UNIQUE INDEX tile_index ON tiles ("
        " zoom_level, tile_column, tile_row)";

    if (SQLITE_OK != sqlite3_exec(database, query.c_str(), 0L, 0L, &errorMsg))
    {
        OE_WARN << LC << "Failed to create index on table [tiles]: " << errorMsg << std::endl;
        sqlite3_free( errorMsg );
        // keep going...
        // return false;
    }

    // TODO: support "grids" and "grid_data" tables if necessary.

    return true;
}


//........................................................................

REGISTER_OSGEARTH_LAYER(mbtileselevation, MBTilesElevationLayer);

OE_LAYER_PROPERTY_IMPL(MBTilesElevationLayer, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(MBTilesElevationLayer, std::string, Format, format);
OE_LAYER_PROPERTY_IMPL(MBTilesElevationLayer, bool, Compress, compress);
OE_LAYER_PROPERTY_IMPL(MBTilesElevationLayer, bool, ComputeLevels, computeLevels);

void
MBTilesElevationLayer::init()
{
    ElevationLayer::init();
    setTileSourceExpected(false);
}

const Status&
MBTilesElevationLayer::open()
{
    if (ElevationLayer::open().isOK())
    {
        // Create an image layer under the hood. TMS fetch is the same for image and
        // elevation; we just convert the resulting image to a heightfield
        _imageLayer = new MBTilesImageLayer(options());

        // Initialize and open the image layer
        _imageLayer->setReadOptions(getReadOptions());
        setStatus( _imageLayer->open() );

        if (getStatus().isOK())
        {
            setProfile(_imageLayer->getProfile());            
        }
    }
    return getStatus();
}

GeoHeightField
MBTilesElevationLayer::createHeightFieldImplementation(const TileKey& key, ProgressCallback* progress) const
{
    // Make an image, then convert it to a heightfield
    GeoImage image = _imageLayer->createImageImplementation(key, progress);
    if (image.valid())
    {
        ImageToHeightFieldConverter conv;
        osg::HeightField* hf = conv.convert( image.getImage() );
        return GeoHeightField(hf, key.getExtent());
    }
    else return GeoHeightField::INVALID;
}