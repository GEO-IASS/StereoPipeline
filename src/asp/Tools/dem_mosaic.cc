// __BEGIN_LICENSE__
//  Copyright (c) 2009-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NGT platform is licensed under the Apache License, Version 2.0 (the
//  "License"); you may not use this file except in compliance with the
//  License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__

/// \file dem_mosaic.cc
///

// A tool to mosaic and blend DEMs, and output the mosaic as tiles.

// Note 1: In practice, the tool may be more efficient if the entire
// mosaic is written out as one single large image, rather than being
// broken up into tiles. To achieve that, just specify to the tool a
// very large tile size, and use 0 for the tile index in the command
// line options.

// Note 2: The tool can be high on memory usage, so processes for
// individual tiles may need to be run on separate machines.

#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <limits>
using namespace std;

#include <vw/FileIO.h>
#include <vw/Image.h>
#include <vw/Cartography.h>
#include <vw/Math.h>
#include <asp/Core/Macros.h>
#include <asp/Core/Common.h>

using namespace vw;
using namespace vw::cartography;

#include <boost/math/special_functions/fpclassify.hpp>
#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include <boost/filesystem/convenience.hpp>
namespace fs = boost::filesystem;

typedef double RealT; // Use double for debugging

// This is used for various tolerances
double g_tol = 1e-6;

// Function for highlighting spots of data
template<class PixelT>
class NotNoDataFunctor {
  typedef typename CompoundChannelType<PixelT>::type channel_type;
  channel_type m_nodata;
  typedef ChannelRange<channel_type> range_type;
public:
  NotNoDataFunctor( channel_type nodata ) : m_nodata(nodata) {}

  template <class Args> struct result {
    typedef channel_type type;
  };

  inline channel_type operator()( channel_type const& val ) const {
    return (val != m_nodata && !isnan(val))? range_type::max() : range_type::min();
  }
};

template <class ImageT, class NoDataT>
UnaryPerPixelView<ImageT,UnaryCompoundFunctor<NotNoDataFunctor<typename ImageT::pixel_type>, typename ImageT::pixel_type>  >
inline notnodata( ImageViewBase<ImageT> const& image, NoDataT nodata ) {
  typedef UnaryCompoundFunctor<NotNoDataFunctor<typename ImageT::pixel_type>, typename ImageT::pixel_type> func_type;
  func_type func( nodata );
  return UnaryPerPixelView<ImageT,func_type>( image.impl(), func );
}

// Set nodata pixels to 0 and valid data pixels to something big.
template<class PixelT>
struct BigOrZero: public ReturnFixedType<PixelT> {
  PixelT m_nodata;
  BigOrZero(PixelT nodata):m_nodata(nodata){}
  double operator() (PixelT const& pix) const {
    if (pix != m_nodata) return 1e+8;
    return 0;
  }
};

BBox2 point_to_pixel_bbox_nogrow(GeoReference const& georef, BBox2 const& ptbox){

  // Given the corners in the projected space, find the pixel corners.
  // This differs from the point_to_pixel_bbox() function in
  // GeoReferenceBase.cc in that in the latter the box is grown to
  // int. Here we prefer finer control.
  
  BBox2 pix_box;
  Vector2 cr[] = {ptbox.min(), ptbox.max(),
                  Vector2(ptbox.min().x(), ptbox.max().y()),
                  Vector2(ptbox.max().x(), ptbox.min().y())};
  for (int icr = 0; icr < (int)(sizeof(cr)/sizeof(Vector2)); icr++)
    pix_box.grow( georef.point_to_pixel(cr[icr]) );

  return pix_box;
}

BBox2 lonlat_to_pixel_bbox_with_adjustment(GeoReference const& georef,
                                           BBox2 lonlat_box){

  // Sometimes a lon-lat box if offset by 360 degrees, in that case we
  // need to fix it before we find the pixel box.
  Vector2 cr = georef.pixel_to_lonlat(Vector2(0, 0));
  int shift = (int)round( (cr[0] - (lonlat_box.min().x()  + lonlat_box.max().x())/2.0 )/360.0 );
  lonlat_box += Vector2(360.0, 0)*shift;

  return  georef.lonlat_to_pixel_bbox(lonlat_box, 1000);
}

GeoReference read_georef(std::string const& file){
  // Read a georef, and check for success
  GeoReference geo;
  bool is_good = read_georeference(geo, file);
  if (!is_good)
    vw_throw(ArgumentErr() << "No georeference found in " << file << ".\n");
  return geo;
}

class DemMosaicView: public ImageViewBase<DemMosaicView>{
  int m_cols, m_rows, m_erode_len, m_blending_len;
  bool m_draft_mode;
  vector< ImageViewRef<RealT> > const& m_images;
  vector<GeoReference> const& m_georefs; 
  GeoReference m_out_georef;
  vector<RealT> m_nodata_values;
  RealT m_out_nodata_value;

public:
  DemMosaicView(int cols, int rows, int erode_len, int blending_len,
                bool draft_mode,
                vector< ImageViewRef<RealT> > const& images,
                vector<GeoReference> const& georefs,
                GeoReference const& out_georef,
                vector<RealT> const& nodata_values, RealT out_nodata_value):
    m_cols(cols), m_rows(rows), m_erode_len(erode_len),
    m_blending_len(blending_len), m_draft_mode(draft_mode),
    m_images(images), m_georefs(georefs),
    m_out_georef(out_georef), m_nodata_values(nodata_values),
    m_out_nodata_value(out_nodata_value){}
  
  typedef RealT pixel_type;
  typedef pixel_type result_type;
  typedef ProceduralPixelAccessor<DemMosaicView> pixel_accessor;
  
  inline int cols() const { return m_cols; }
  inline int rows() const { return m_rows; }
  inline int planes() const { return 1; }
  
  inline pixel_accessor origin() const { return pixel_accessor( *this, 0, 0 ); }

  inline pixel_type operator()( double/*i*/, double/*j*/, int/*p*/ = 0 ) const {
    vw_throw(NoImplErr() << "DemMosaicView::operator()(...) is not implemented");
    return pixel_type();
  }
  
  typedef CropView<ImageView<pixel_type> > prerasterize_type;
  inline prerasterize_type prerasterize(BBox2i const& bbox) const {

    // We will do all computations in double precision, regardless
    // of the precision of the inputs, for increased accuracy.
    typedef PixelGrayA<double> RealGrayA;
    ImageView<double> tile(bbox.width(), bbox.height());
    ImageView<double> weights(bbox.width(), bbox.height());
    fill( tile, m_out_nodata_value );
    fill( weights, 0.0 );
    
    for (int dem_iter = 0; dem_iter < (int)m_images.size(); dem_iter++){
      
      GeoReference georef = m_georefs[dem_iter];
      ImageViewRef<double> disk_dem = pixel_cast<double>(m_images[dem_iter]);
      double nodata_value = m_nodata_values[dem_iter];

      // It is very important that all computations be done below in
      // point units the projected space, rather than in lon-lat. The
      // latter can break down badly around poles.
      
      // The tile corners as pixels in curr_dem
      BBox2 point_box = m_out_georef.pixel_to_point_bbox(bbox);
      BBox2 pix_box = point_to_pixel_bbox_nogrow(georef, point_box);
      pix_box.min() = floor(pix_box.min());
      pix_box.max() = ceil(pix_box.max());

      // Grow to account for blending and erosion length, etc.
      pix_box.expand(m_erode_len + m_blending_len + BilinearInterpolation::pixel_buffer + 1);
      pix_box.crop(bounding_box(disk_dem));
      if (pix_box.empty()) continue;

      // Crop the disk dem to a 2-channel in-memory image. First channel
      // is the image pixels, second will be the grassfire weights.
      ImageView<RealGrayA> dem = crop(disk_dem, pix_box);

      // Use grassfire weights for smooth blending
      ImageView<double> local_wts;
      if (m_draft_mode)
        local_wts = per_pixel_filter(select_channel(dem, 0),
                                     BigOrZero<double>(nodata_value));
      else
        local_wts = grassfire(notnodata(select_channel(dem, 0),
                                        nodata_value));

      // Dump the weights
      //std::ostringstream os;
      //os << "weights_" << dem_iter << ".tif";
      //std::cout << "Writing: " << os.str() << std::endl;
      //block_write_gdal_image(os.str(), local_wts, georef, -100,
      //                       asp::BaseOptions(),
      //                       TerminalProgressCallback("asp", ""));

      int max_cutoff = max_pixel_value(local_wts);
      int min_cutoff = m_erode_len;
      if (max_cutoff <= min_cutoff) max_cutoff = min_cutoff + 1; // precaution
      
      // Erode
      local_wts = clamp(local_wts - min_cutoff, 0.0, max_cutoff - min_cutoff);
    
      // Set the weights in the alpha channel
      for (int col = 0; col < dem.cols(); col++){
        for (int row = 0; row < dem.rows(); row++){
          dem(col, row).a() = local_wts(col, row);
        }
      }
      
      ImageViewRef<RealGrayA> interp_dem
        = interpolate(dem, BilinearInterpolation(), ConstantEdgeExtension());
      
      for (int c = 0; c < bbox.width(); c++){
        for (int r = 0; r < bbox.height(); r++){
          Vector2 out_pix(c +  bbox.min().x(), r +  bbox.min().y());
          Vector2 in_pix = georef.point_to_pixel
            (m_out_georef.pixel_to_point(out_pix));

          double x = in_pix[0] - pix_box.min().x();
          double y = in_pix[1] - pix_box.min().y();
          RealGrayA pval;

          int i0 = round(x), j0 = round(y);
          if (fabs(x-i0) < g_tol && fabs(y-j0) < g_tol &&
              (i0 >= 0 && i0 <= dem.cols()-1 &&
               j0 >= 0 && j0 <= dem.rows()-1) ){

            // A lot of care is needed here. We are at an integer
            // pixel, save for numerical error. Just borrow pixel's
            // value, and don't interpolate. Interpolation can result
            // in invalid pixels if the current pixel is valid but its
            // neighbors are not. It can also make it appear is if the
            // current point is out of bounds while in fact it is
            // barely so.
            pval = dem(i0, j0);
            
          }else{
            
            // below must use x <= cols()-1 as x is double
            bool is_good = (x >= 0 && x <= dem.cols()-1 &&
                            y >= 0 && y <= dem.rows()-1);
            if (!is_good) continue;

            // If we have weights of 0, that means there are invalid
            // pixels, so skip this point.
            int i = (int)floor(x), j = (int)floor(y);
            if (dem(i, j  ).a() <= 0 || dem(i+1, j  ).a() <= 0 ||
                dem(i, j+1).a() <= 0 || dem(i+1, j+1).a() <= 0) continue;
            pval = interp_dem(x, y);
          }
          double val = pval.v();
          double wt = pval.a();
          
          if (wt <= 0) continue;

          // Initialize the tile if not done already
          if ( tile(c, r) == m_out_nodata_value || isnan(tile(c, r)) )
            tile(c, r) = 0;
          
          if (!m_draft_mode){
            // Combine the values
            tile(c, r) += wt*val;
            weights(c, r) += wt;
          }else{
            // Use just the last value
            tile(c, r) = val;
            weights(c, r) = 1;
          }
          
        }
      }
      
    } // end iterating over DEMs
    
    // Divide by the weights
    int num_valid_pixels = 0;
    for (int c = 0; c < bbox.width(); c++){
      for (int r = 0; r < bbox.height(); r++){
        if ( weights(c, r) > 0 ){
          tile(c, r) /= weights(c, r);
          num_valid_pixels++;
        }
      }
    }

    return prerasterize_type(pixel_cast<RealT>(tile), -bbox.min().x(), -bbox.min().y(),
                             cols(), rows() );
  }

  template <class DestT>
  inline void rasterize(DestT const& dest, BBox2i bbox) const {
    vw::rasterize(prerasterize(bbox), dest, bbox);
  }
};

std::string processed_proj4(std::string const& srs){
  // Apparently functionally identical proj4 strings can differ in
  // subtle ways, such as an extra space, etc. For that reason, must
  // parse and process any srs string before comparing it with another
  // string.
  GeoReference georef;
  bool have_user_datum = false;
  Datum user_datum;
  asp::set_srs_string(srs,
                      have_user_datum, user_datum, georef);
  return georef.proj4_str();
}

struct Options : asp::BaseOptions {
  bool draft_mode;
  string dem_list_file, out_prefix, target_srs_string;
  vector<string> dem_files;
  double mpp, tr, geo_tile_size;
  bool has_out_nodata;
  RealT out_nodata_value;
  int tile_size, tile_index, erode_len, blending_len;
  Options(): geo_tile_size(0), has_out_nodata(false), tile_index(-1){}
};

void handle_arguments( int argc, char *argv[], Options& opt ) {
  
  po::options_description general_options("Options");
  general_options.add_options()
    ("dem-list-file,l", po::value<string>(&opt.dem_list_file),
     "Text file listing the DEM files to mosaic, one per line.")
    ("output-prefix,o", po::value(&opt.out_prefix), "Specify the output prefix.")
    ("tile-size", po::value<int>(&opt.tile_size)->default_value(1000000),
     "The maximum size of output DEM tile files to write, in pixels.")
    ("tile-index", po::value<int>(&opt.tile_index),
     "The index of the tile to save (starting from zero). When this program is invoked, it will print  out how many tiles are there. Default: save all tiles.")
    ("erode-length", po::value<int>(&opt.erode_len)->default_value(0),
     "Erode input DEMs by this many pixels at boundary and hole edges before mosacking them.")
    ("blending-length", po::value<int>(&opt.blending_len)->default_value(200),
     "Larger values of this number (measured in input DEM pixels) may result in smoother blending while using more memory and computing time.")
    ("tr", po::value(&opt.tr)->default_value(0.0),
     "Output DEM resolution in target georeferenced units per pixel. If not specified, use the same resolution as the first DEM to be mosaicked.")
    ("t_srs", po::value(&opt.target_srs_string)->default_value(""),
     "Specify the projection (PROJ.4 string). If not provided, use the one from the first DEM to be mosaicked.")
    ("georef-tile-size", po::value<double>(&opt.geo_tile_size),
     "Set the tile size in georeferenced (projected) units (e.g., degrees or meters).")
    ("output-nodata-value", po::value<RealT>(&opt.out_nodata_value),
     "No-data value to use on output. If not specified, use the one from the first DEM to be mosaicked.")
    ("draft-mode", po::bool_switch(&opt.draft_mode)->default_value(false),
     "Put the DEMs together without blending them (the result is less smooth).")
    ("threads", po::value<int>(&opt.num_threads),
     "Number of threads to use.")
    ("help,h", "Display this help message.");

  // Parse options
  po::options_description options("Allowed Options");
  options.add(general_options);

  po::options_description positional("");
  po::positional_options_description positional_desc;
    
  std::string usage("[options] <dem files or -l dem_file_list.txt> -o output_file_prefix");
  bool allow_unregistered = true;
  std::vector<std::string> unregistered;
  po::variables_map vm =
    asp::check_command_line( argc, argv, opt, general_options, general_options,
                             positional, positional_desc, usage,
                             allow_unregistered, unregistered );

  // Error checking  
  if (opt.mpp > 0.0 && opt.tr > 0.0)
    vw_throw(ArgumentErr() << "Just one of the --mpp and --tr options needs to be set.\n"
              << usage << general_options );
  if (opt.out_prefix == "")
    vw_throw(ArgumentErr() << "No output prefix was specified.\n"
              << usage << general_options );
  if (opt.num_threads == 0)
    vw_throw(ArgumentErr() << "The number of threads must be set and "
             << "positive.\n" << usage << general_options );
  if (opt.erode_len < 0)
    vw_throw(ArgumentErr() << "The erode length must not be negative.\n"
             << usage << general_options );
  if (opt.blending_len < 0)
    vw_throw(ArgumentErr() << "The blending length must not be negative.\n"
             << usage << general_options );
  if (opt.tile_size <= 0)
    vw_throw(ArgumentErr() << "The size of a tile in pixels must "
             << "be set and positive.\n"
              << usage << general_options );
  if (opt.draft_mode && opt.erode_len > 0)
    vw_throw(ArgumentErr() << "Cannot erode pixels in draft mode.\n"
             << usage << general_options );
  if (opt.geo_tile_size < 0)
    vw_throw(ArgumentErr() << "The size of a tile in georeferenced units must "
             << "not be negative.\n"
              << usage << general_options );

  // Read the DEMs
  if (opt.dem_list_file != ""){

    // Get them from a list
    
    if (!unregistered.empty())
      vw_throw(ArgumentErr() << "The DEMs were specified via a list. There were however "
               << "extraneous files or options passed in.\n"
               << usage << general_options );
    
    ifstream is(opt.dem_list_file.c_str());
    string file;
    while (is >> file) opt.dem_files.push_back(file);
    if (opt.dem_files.empty())
      vw_throw(ArgumentErr() << "No DEM files to mosaic.\n");
    is.close();

  }else{

    // Get them from the command line
    if (unregistered.empty())
      vw_throw(ArgumentErr() << "No input DEMs were specified..\n"
               << usage << general_options );
    opt.dem_files = unregistered;
  }
  
  // Create the output directory 
  asp::create_out_dir(opt.out_prefix);
  
  // Turn on logging to file
  asp::log_to_file(argc, argv, "", opt.out_prefix);

  if (!vm.count("output-nodata-value")){
    // Set a default out_nodata_value, but remember that this is
    // set internally, not by the user.
    opt.has_out_nodata = false;
    opt.out_nodata_value= -numeric_limits<RealT>::max();
  }else
    opt.has_out_nodata = true;

}

int main( int argc, char *argv[] ) {

  Options opt;
  try{
    
    handle_arguments( argc, argv, opt );

    // Read nodata from first DEM, unless the user chooses to specify it.
    if (!opt.has_out_nodata){
      DiskImageResourceGDAL in_rsrc(opt.dem_files[0]);
      if (in_rsrc.has_nodata_read()) opt.out_nodata_value = in_rsrc.nodata_read();
    }
    vw_out() << "Using output no-data value: " << opt.out_nodata_value << endl;

    // Form the mosaic georef. The georef of the first DEM is used as
    // initial guess unless user wants to change the resolution and
    // projection.
    if (opt.target_srs_string != "")
      opt.target_srs_string = processed_proj4(opt.target_srs_string);
      
    GeoReference out_georef = read_georef(opt.dem_files[0]);
    double spacing = opt.tr;
    if (opt.target_srs_string != ""                     &&
        opt.target_srs_string != out_georef.proj4_str() &&
        spacing <= 0 ){
      vw_throw(ArgumentErr()
               << "Changing the projection was requested. The output DEM "
               << "resolution must be specified via the --tr option.\n");
    }

    if (opt.target_srs_string != ""){
      // Set the srs string into georef.
      bool have_user_datum = false;
      Datum user_datum;
      asp::set_srs_string(opt.target_srs_string,
                          have_user_datum, user_datum, out_georef);
    }
    
    // Use desired spacing if user-specified
    if (spacing > 0.0){
      Matrix<double,3,3> transform = out_georef.transform();
      transform.set_identity();
      transform(0, 0) = spacing;
      transform(1, 1) = -spacing;
      out_georef.set_transform(transform);
    }else
      spacing = out_georef.transform()(0, 0);

    if (opt.geo_tile_size > 0){
      opt.tile_size = (int)round(opt.geo_tile_size/spacing);
      vw_out() << "Tile size in pixels: " << opt.tile_size << "\n";
    }
    opt.tile_size = std::max(opt.tile_size, 1);
    
    // Store the no-data values, pointers to images, and georeferences
    // (for speed). Find the bounding box of all DEMs in the projected
    // space.
    vw_out() << "Reading the input DEMs.\n";
    TerminalProgressCallback tpc("", "\t--> ");
    tpc.report_progress(0);
    double inc_amount = 1.0 / double(opt.dem_files.size() );
    vector<RealT> nodata_values;
    vector< ImageViewRef<RealT> > images;
    vector< GeoReference > georefs;
    BBox2 mosaic_bbox;
    for (int dem_iter = 0; dem_iter < (int)opt.dem_files.size(); dem_iter++){
      
      double curr_nodata_value = opt.out_nodata_value;
      DiskImageResourceGDAL in_rsrc(opt.dem_files[dem_iter]);
      if ( in_rsrc.has_nodata_read() ) curr_nodata_value = in_rsrc.nodata_read();
      nodata_values.push_back(curr_nodata_value);

      GeoReference georef = read_georef(opt.dem_files[dem_iter]);
      if (out_georef.proj4_str() == georef.proj4_str()){
        images.push_back(DiskImageView<RealT>( opt.dem_files[dem_iter] ));
      }else{
        // Need to reproject and change the reference.
        GeoReference oldgeo = georef;
        GeoReference newgeo = out_georef;
        
        DiskImageView<RealT> img(opt.dem_files[dem_iter]);
        BBox2 imgbox = bounding_box(img);
        BBox2 lonlat_box = oldgeo.pixel_to_lonlat_bbox(imgbox);
        BBox2 pixbox = lonlat_to_pixel_bbox_with_adjustment(newgeo, lonlat_box);
        newgeo = crop(newgeo, pixbox.min().x(), pixbox.min().y());

        cartography::GeoTransform trans(oldgeo, newgeo);
        BBox2 output_bbox = trans.forward_bbox( imgbox );
        typedef PixelMask<RealT> PMaskT;
        
        ImageViewRef<RealT> trans_img
          = apply_mask
          (crop( transform( create_mask(img, curr_nodata_value), trans,
                            ValueEdgeExtension<PMaskT>(PMaskT()), BilinearInterpolation() ),
                 output_bbox ),
           curr_nodata_value);
        
        images.push_back(trans_img);
        georef = newgeo;
      }
      georefs.push_back(georef);
      
      mosaic_bbox.grow(georefs[dem_iter].bounding_box(images[dem_iter]));
      tpc.report_incremental_progress( inc_amount );
    }
    tpc.report_finished();


    // Set the lower-left corner. Note: The position of the corner is
    // somewhat arbitrary. If the corner is actually very close to an
    // integer number, we assume it should in fact be integer but got
    // moved a bit due to numerical error. Then we set it to
    // integer. This ensures that when we mosaic a single DEM we get
    // its corners to be the same as the originals rather than moved
    // by a slight offset.
    BBox2 pixel_box = point_to_pixel_bbox_nogrow(out_georef, mosaic_bbox);
    Vector2 beg_pix = pixel_box.min();
    if (norm_2(beg_pix - round(beg_pix)) < g_tol ) beg_pix = round(beg_pix);
    out_georef = crop(out_georef, beg_pix[0], beg_pix[1]);

    // Image size
    pixel_box = point_to_pixel_bbox_nogrow(out_georef, mosaic_bbox);
    Vector2 end_pix = pixel_box.max();
    
    int cols = (int)round(end_pix[0]); // end_pix is the last pix in the image
    int rows = (int)round(end_pix[1]);
    
    // Form the mosaic and write it to disk
    vw_out()<< "The size of the mosaic is " << cols << " x " << rows
            << " pixels.\n";

    int num_tiles_x = (int)ceil((double)cols/double(opt.tile_size));
    if (num_tiles_x <= 0) num_tiles_x = 1;
    int num_tiles_y = (int)ceil((double)rows/double(opt.tile_size));
    if (num_tiles_y <= 0) num_tiles_y = 1;
    int num_tiles = num_tiles_x*num_tiles_y;
    vw_out() << "Number of tiles: " << num_tiles_x << " x "
             << num_tiles_y << " = " << num_tiles << std::endl;

    // The next power of 2 >= 4*(blending_len + erode_len). We want to
    // make the blocks big, to reduce overhead from blending_len and
    // erode_len, but not so big that it may not fit in memory.
    int block_size = ( 1 << (int)ceil(log(4.0*max(1, opt.erode_len
                                                  + opt.blending_len))/log(2.0)) );
    block_size = std::max(block_size, 256); // don't make them too small though
    

    if (opt.tile_index >= num_tiles){
      vw_out() << "Tile with index: " << opt.tile_index << " is out of bounds."
               << std::endl;
      return 0;
    }

    // See if to save all tiles, or an individual tile.
    int start_tile = opt.tile_index, end_tile = opt.tile_index + 1;
    if (opt.tile_index < 0){
      start_tile = 0;
      end_tile = num_tiles;
    }
    
    for (int tile_id = start_tile; tile_id < end_tile; tile_id++){
      
      int tile_index_y = tile_id / num_tiles_y;
      int tile_index_x = tile_id - tile_index_y*num_tiles_y;
      BBox2i tile_box(tile_index_x*opt.tile_size, tile_index_y*opt.tile_size,
                      opt.tile_size, opt.tile_size);
      tile_box.crop(BBox2i(0, 0, cols, rows));
      ostringstream os; os << opt.out_prefix << "-tile-" << tile_id << ".tif";
      std::string dem_tile = os.str();
      
      // We use block_cache to rasterize tiles of size block_size.
      ImageViewRef<RealT> out_dem = 
        block_cache(crop(DemMosaicView(cols, rows, opt.erode_len, opt.blending_len,
                                       opt.draft_mode, images, georefs,
                                       out_georef, nodata_values,
                                       opt.out_nodata_value),
                         tile_box),
                    Vector2(block_size, block_size), opt.num_threads);

      if (out_dem.cols() == 0 || out_dem.rows() == 0){
        vw_out() << "Skip writing empty image: " << dem_tile << std::endl;
        continue;
      }
      
      vw_out() << "Writing: " << dem_tile << std::endl;
      GeoReference crop_georef
        = crop(out_georef, tile_box.min().x(), tile_box.min().y());
      block_write_gdal_image(dem_tile, out_dem, crop_georef, opt.out_nodata_value,
                             opt, TerminalProgressCallback("asp", "\t--> "));
    }
    
  } ASP_STANDARD_CATCHES;
  
  return 0;
}
