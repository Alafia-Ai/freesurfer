#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#include "mri.h"
#include "const.h"
#include "diag.h"
#include "error.h"
#include "macros.h"
#include "proto.h"
#include "mrimorph.h"
#include "timer.h"
#include "mrinorm.h"
#include "cma.h"
#include "version.h"
#include "error.h"
#include "volume_io/geom_structs.h"
#include "transform.h"
#include "talairachex.h"

static char vcid[] = "$Id: mri_fill.c,v 1.81 2004/07/30 13:39:17 tosa Exp $";


/*-------------------------------------------------------------------
                                CONSTANTS
-------------------------------------------------------------------*/

#define DEBUG_POINT(x,y,z)  (((x==140) && (y==74)) &&((z)==174))

#define PONS_LOG_FILE                   "pons.log"
#define CC_LOG_FILE                     "cc.log"

/* min # of neighbors which must be on to retain a point */
#define DEFAULT_NEIGHBOR_THRESHOLD      1


/* distance to search in each direction for a valid wm seed point */
#define SEED_SEARCH_SIZE                9

/* size of various orthogonal slices */
#define SLICE_SIZE                      255

/* the min # of neighbors on for a point to be allowed to be a seed point */
#define MIN_NEIGHBORS                    5

/* Talairach seed points - only used if heuristics fail */
#define CORPUS_CALLOSUM_TAL_X            0.0
#define CORPUS_CALLOSUM_TAL_Y            0.0
#define CORPUS_CALLOSUM_TAL_Z            27.0

#define PONS_TAL_X                      -2.0
#define PONS_TAL_Y                      -15.0  /* was -22.0 */
#define PONS_TAL_Z                      -17.0

#define WHITE_MATTER_RH_TAL_X            29.0
#define WHITE_MATTER_RH_TAL_Y            -12.0 ;
#define WHITE_MATTER_RH_TAL_Z            28.0 ;


#define WHITE_MATTER_LH_TAL_X            -29.0
#define WHITE_MATTER_LH_TAL_Y            -12.0 ;
#define WHITE_MATTER_LH_TAL_Z            28.0 ;


/* # if iterations for filling */
#define MAX_ITERATIONS  1000  /* was 10 */

/* min # of holes filled before quitting */
#define MIN_FILLED      0     /* was 100 */

#define FILLED_PRIORS_FNAME  "talairach_hemi_filled.mgh"

/*-------------------------------------------------------------------
                                GLOBAL DATA
-------------------------------------------------------------------*/

static FILE *log_fp = NULL ;
static int lh_fill_val = MRI_LEFT_HEMISPHERE ;
static int rh_fill_val = MRI_RIGHT_HEMISPHERE ;

static int ylim0,ylim1,xlim0,xlim1;
static int fill_holes_flag = TRUE;

/* corpus callosum seed point in Talairach coords */

static Real cc_tal_x = 0.0 ;
static Real cc_tal_y = 0.0 ;
static Real cc_tal_z = 27.0 ;

static Real pons_tal_x = -2.0 ;
static Real pons_tal_y = -15.0 /* -22.0 */ ;
static Real pons_tal_z = -17.0 ;

static int cc_seed_set = 0 ;
static int pons_seed_set = 0 ;

// seed specified in volume position ///////////////
static Real cc_vol_x = 0;
static Real cc_vol_y = 0;
static Real cc_vol_z = 0;

static Real pons_vol_x = 0;
static Real pons_vol_y = 0;
static Real pons_vol_z = 0;

static int cc_seed_vol_set = 0;
static int pons_seed_vol_set = 0;
////////////////////////////////////////////////////

static int lh_seed_set = 0 ;
static int rh_seed_set = 0 ;

static Real lh_tal_x ;
static Real lh_tal_y ;
static Real lh_tal_z ;

static Real rh_tal_x ;
static Real rh_tal_y ;
static Real rh_tal_z ;

char *Progname ;

static int min_filled = 0 ;

static int neighbor_threshold = DEFAULT_NEIGHBOR_THRESHOLD ;

static MRI *mri_fill, *mri_im ;

static int logging = 0 ;

static int fill_val = 0 ;   /* only non-zero for generating images of planes */
static int cc_mask = 1 ;

static char *sname ;

static char *atlas_name = NULL ;
#if 0
static float blur_sigma = 0.25f ;
#endif

// use xform and its dst offset for Talairach volume
static LTA *lta = 0;

static int labels[] =   
{ THICKEN_FILL, NBHD_FILL, VENTRICLE_FILL, DIAGONAL_FILL, DEGENERATE_FILL };
#define NLABELS  sizeof(labels) / (sizeof(labels[0]))

static char *segmentation_fname = NULL ;


double findMinSize(MRI *mri)
{
  double xsize, ysize, zsize, minsize;
  xsize = mri->xsize;
  ysize = mri->ysize;
  zsize = mri->zsize;
  // there are 3! = 6 ways of ordering
  //             xy  yz  zx
  // x > y > z    z min
  // x > z > y    y min  
  // z > x > y    y min
  //////////////////////////
  // y > x > z    z min
  // y > z > x    x min
  // z > y > x    x min
  if (xsize > ysize)
    minsize = (ysize > zsize) ? zsize : ysize;
  else
    minsize = (zsize > xsize) ? xsize : zsize;

  return minsize;
}

int verifyLRSplit(MRI *mri_fill, LTA *lta, Real cc_tal_x, int *pbadRH, int *pbadLH, int *ptotRH, int *ptotLH)
{
  int x, y, z;
  Real tal_x, tal_y, tal_z;
  unsigned char val;
  // gets linear transform and thus fill val may have non rh_fill_val or lh_fill_val
  int badRH = 0;
  int badLH = 0;
  int RH = 0;
  int LH = 0;
  for (z =0; z < mri_fill->depth; ++z)
    for (y=0; y < mri_fill->height; ++y)
      for (x=0; x < mri_fill->width; ++x)
      {
	val = MRIvox(mri_fill, x, y, z);
	if (val != 0)
	{
	  if (val == rh_fill_val)
	    RH++;
	  else if (val==lh_fill_val)
	    LH++;
	  // get the talairach coordinate position
	  MRIvoxelToTalairachEx(mri_fill, x, y, z, &tal_x, &tal_y, &tal_z, lta);
	  if (tal_x < cc_tal_x) // talairach coordinates less means left
	  {  
	    // val must be lh_fill_val, except the case when the brain bends
	    if (val == rh_fill_val)
	    {
	      badRH++;
	      // if (badRH < 10)
	      //   fprintf(stderr, "badRH: (%d, %d, %d)\n", x, y, z);
	    }
	  }
	  else if (tal_x > cc_tal_x) // talairach coordinate greater means right
	  {
	    // val must be rh_fill_val, except the case when the brain bends
	    if (val == lh_fill_val)
	    {
	      badLH++;
	      // if (badLH < 10)
	      //   fprintf(stderr, "badLH: (%d, %d, %d)\n", x, y, z);
	    }
	  }
	}
      }

  // if (badRH > 10 || badLH > 10)
  //   fprintf(stderr, "...\n");

  *pbadRH = badRH;
  *pbadLH = badLH;
  *ptotRH = RH;
  *ptotLH = LH;

  return (NO_ERROR);
}

/*-------------------------------------------------------------------
                             STATIC PROTOTYPES
-------------------------------------------------------------------*/

static int MRIlabelsInNbhd(MRI *mri, int x, int y, int z, int whalf, int label) ;
#if 0
static int neighbors(MRI *mri, int x, int y,int z,int whalf,int label);
#endif
static MRI *extend_to_lateral_borders(MRI *mri_src, MRI *mri_dst, int mask) ;
static int neighborLabel(MRI *mri, int x, int y, int z, int whalf, int label);
static int edit_segmentation(MRI *mri_im, MRI *mri_seg) ;
static int get_option(int argc, char *argv[]) ;
static void print_version(void) ;
static void print_help(void) ;
int main(int argc, char *argv[]) ;

static int fill_holes(MRI *mri_fill) ;
static int fill_brain(MRI *mri_fill, MRI *mri_im, int threshold) ;
static MRI *find_cutting_plane(MRI *mri, Real x_tal, Real y_tal,Real z_tal,
                              int orientation, int *pxv, int *pvy, int *pzv,
                               int seed_set, const LTA *lta) ;
static int find_slice_center(MRI *mri,  int *pxo, int *pyo) ;
static int find_cc_seed_with_segmentation(MRI *mri_tal, MRI *mri_seg, Real *pcc_tal_x, Real *cc_tal_y, Real *cc_tal_z) ;
static int find_corpus_callosum(MRI *mri, Real *ccx, Real *ccy, Real *ccz, const LTA *lta) ;
static int find_pons(MRI *mri, Real *p_ponsx, Real *p_ponsy, Real *p_ponsz,
                     int x_cc, int y_cc, int z_cc, int which) ;
static int find_cc_slice(MRI *mri, Real *pccx, Real *pccy, Real *pccz, const LTA *lta) ;
static int neighbors_on(MRI *mri, int x0, int y0, int z0) ;
static int MRIfillVolume(MRI *mri_fill, MRI *mri_im, int x_seed, int y_seed, 
                         int z_seed, int fill_val) ;
static int   mriFindBoundingBox(MRI *mri_im) ;
MRI *MRIcombineHemispheres(MRI *mri_lh_fill, MRI *mri_rh_fill, MRI *mri_dst,
                      int wm_lh_x, int wm_lh_y, int wm_lh_z,
                      int wm_rh_x, int wm_rh_y, int wm_rh_z) ;
static MRI *mriReadBinaryProbabilities(char *atlas_name, char *suffix, 
                                       M3D *m3d, char *subject_name, 
                                       MRI *mri_dst) ;
static MRI *mriReadConditionalProbabilities(MRI *mri_T1, char *atlas_name, 
                                            char *suffix, int offset, 
                                            M3D *m3d, MRI *mri_dst) ;
static MRI *MRIfillVentricle(MRI *mri_src, MRI *mri_prob, MRI *mri_T1,
                             MRI *mri_dst, float thresh, int out_label,
                             int xmin, int xmax) ;
static int MRIfillDegenerateLocations(MRI *mri_fill, int fillval) ;

/*-------------------------------------------------------------------
                                FUNCTIONS
-------------------------------------------------------------------*/
int
main(int argc, char *argv[])
{
  int     x, y, z, xd, yd, zd, xnew, ynew, znew, msec, i, found ;
  int     nargs, wm_rh_x, wm_rh_y, wm_rh_z, wm_lh_x, wm_lh_y, wm_lh_z ;
  char    input_fname[STRLEN],out_fname[STRLEN], fname[STRLEN] ;
  Real    xr, yr, zr, dist, min_dist ;
  MRI     *mri_cc, *mri_pons, *mri_lh_fill, *mri_rh_fill, *mri_lh_im, 
          *mri_rh_im /*, *mri_blur*/, *mri_labels, *mri_tal, *mri_tmp, *mri_tmp2,
          *mri_saved_labels, *mri_seg ;
  int     x_pons, y_pons, z_pons, x_cc, y_cc, z_cc, xi, yi, zi ;
  MORPH_3D  *m3d ;
  struct timeb  then ;
  int seed_search_size;
  double voxsize;
  int badRH, badLH;

  // new
  MRI *mri_talheader;
  LT *lt;
  MATRIX *m_L;
  int row;
  VOL_GEOM *dst=0;
  VOL_GEOM *src=0;

  // Gdiag = 0xFFFFFFFF;

  /* rkt: check for and handle version tag */
  nargs = handle_version_option (argc, argv, "$Id: mri_fill.c,v 1.81 2004/07/30 13:39:17 tosa Exp $", "$Name:  $");
  if (nargs && argc - nargs == 1)
    exit (0);
  argc -= nargs;

  TimerStart(&then) ;
  DiagInit(NULL, NULL, NULL) ;
  ErrorInit(NULL, NULL, NULL) ;

  Progname = argv[0] ;

  for ( ; argc > 1 && (*argv[1] == '-') ; argc--, argv++)
  {
    nargs = get_option(argc, argv) ;
    argc -= nargs ;
    argv += nargs ;
  }
  
  if (argc < 3)
    print_help() ;   /* will exit */

  strcpy(input_fname, argv[1]) ;
  strcpy(out_fname, argv[2]) ;

  if (!Gdiag)
    fprintf(stderr, "reading input volume...") ;

  mri_im = MRIread(input_fname) ;
  if (!mri_im)
    ErrorExit(ERROR_NOFILE, "%s: could not read %s", Progname, input_fname) ;

  /* store all the things that are labeled other than white matter,
     and erase them so they don't confuse the issue of finding the cc
     and the pons. They will be restored later, before doing the fill.
  */
  mri_labels = MRIclone(mri_im, NULL) ;
  for (i = 0 ; i < NLABELS ; i++)
  {
    MRIcopyLabel(mri_im, mri_labels, labels[i]) ;
    MRIreplaceValues(mri_im, mri_im, labels[i], 0) ;
  }
  mri_saved_labels = MRIcopy(mri_labels, NULL) ;

  if (atlas_name && 0)
  {
    MRI *mri_p_wm ;
    char subjects_dir[STRLEN], mri_dir[STRLEN], *cp ;

    cp = getenv("MRI_DIR") ;
    if (!cp)
      ErrorExit(ERROR_BADPARM, "%s: could not read MRI_DIR from environment",
                Progname) ;
    strcpy(mri_dir, cp) ;

    cp = getenv("SUBJECTS_DIR") ;
    if (!cp)
      ErrorExit(ERROR_BADPARM, 
                "%s: could not read SUBJECTS_DIR from environment",Progname) ;
    strcpy(subjects_dir, cp) ;


    sprintf(fname, "%s/%s/mri/transforms/%s.m3d", subjects_dir,sname,
            atlas_name) ;
    m3d = MRI3DreadSmall(fname) ;
    if (!m3d)
      ErrorExit(ERROR_NOFILE, 
                "%s: could not open transform file %s\n",
                Progname, fname) ;
    fprintf(stderr, "removing extraneous white matter using atlas %s...\n",
            atlas_name) ;
    mri_p_wm = mriReadBinaryProbabilities(atlas_name, "wm", m3d, sname, NULL);
    if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
      MRIwrite(mri_p_wm, "p_wm.mgh") ;

    MRIprobabilityThresholdNeighborhoodOff(mri_im, mri_p_wm, mri_im, 0.0, 3);
    if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
      MRIwrite(mri_im, "wm_thresh.mgh") ;

#if 0
    fprintf(stderr, "filling high probability white matter...\n") ;
    mri_blur = MRIgaussian1d(blur_sigma, 0) ;
    if (!mri_blur)
      ErrorExit(ERROR_BADPARM, 
                "%s: could not allocate blurring kernel with sigma=%2.3f",
                Progname, blur_sigma) ;
    mri_tmp = MRIconvolveGaussian(mri_p_wm, NULL, mri_blur) ;
    MRIfree(&mri_p_wm) ; mri_p_wm = mri_tmp ;
    MRIprobabilityThreshold(mri_im, mri_p_wm, mri_im, 98.0, 255) ;
#endif

    MRIfree(&mri_p_wm) ;
    MRI3DmorphFree(&m3d) ;
  }

  if (!Gdiag)
    fprintf(stderr, "done.\nsearching for cutting planes...") ;

  ////////////////////////////////////////////////////////////////////////////
  // set up mri_talheader
  ////////////////////////////////////////////////////////////////////////////
  mri_talheader = MRIallocHeader(mri_im->width, mri_im->height, mri_im->depth, mri_im->type);
  MRIcopyHeader(mri_im, mri_talheader); // not allocate memory, though
  // modify c_(r,a,s) depending on the xfm dst value

  //////////////////////////////////////////////////////////////////////////
  // Make sure that we have lta
  /////////////////////////////////////////////////////////////////////////
  // if lta is loaded
  if (lta)
  {
    ModifyTalairachCRAS(mri_talheader, lta);
  }
  else // lta is not loaded and thus we create 
  {
    lta = LTAalloc(1, NULL) ;
    lt = &lta->xforms[0] ;
    lt->sigma = 1.0f ;
    lt->x0 = lt->y0 = lt->z0 = 0 ;
    m_L = lt->m_L = MatrixIdentity(4, NULL);
    lta->type = LINEAR_RAS_TO_RAS;
    // try getting from mri
    // transform is MNI transform (only COR volume reads transform) 
    if (mri_im->linear_transform)
    {
      // linear_transform is zero based column-major array
      for (row = 1 ; row <= 3 ; row++)
      {
	*MATRIX_RELT(m_L,row,1) = mri_im->linear_transform->m[0][row-1];
	*MATRIX_RELT(m_L,row,2) = mri_im->linear_transform->m[1][row-1];
	*MATRIX_RELT(m_L,row,3) = mri_im->linear_transform->m[2][row-1];
	*MATRIX_RELT(m_L,row,4) = mri_im->linear_transform->m[3][row-1];
      }
      fprintf(stderr, "talairach transform\n");
      MatrixPrint(stderr, m_L);
      dst = &lt->dst;
      src = &lt->src;
      getVolGeom(mri_im, src);
      getVolGeom(mri_im, dst);
      if (getenv("NO_AVERAGE305")) // if this is set
	fprintf(stderr, "INFO: tal dst c_(r,a,s) not modified\n");
      else
      {
	fprintf(stderr, "INFO: Modifying talairach volume c_(r,a,s) based on average_305\n");
	dst->valid = 1;
	// use average_305 value
	dst->c_r = -0.095;
	dst->c_a = -16.51;
	dst->c_s =   9.75;
	mri_talheader->c_r = dst->c_r;
	mri_talheader->c_a = dst->c_a;
	mri_talheader->c_s = dst->c_s;
      }
    }
    else
    {
      printf("INFO: volume does not have linear_transform set nor lta is given by option.-xform.\n") ;
      printf("INFO: use identity matrix as the talairach transform.\n");
      printf("INFO: use src volume parameters for the talairach volume.\n");
      dst = &lt->dst;
      src = &lt->src;
      getVolGeom(mri_im, src);
      getVolGeom(mri_im, dst);
      dst->valid = 1;
      mri_talheader->c_r = dst->c_r;
      mri_talheader->c_a = dst->c_a;
      mri_talheader->c_s = dst->c_s;
    }
    // when modify c_(ras), you must recalculate i_to_r__ and r_to_i__
    // when you modify c_(ras), you must recalculate i_to_r__ and r_to_i__
    if (mri_talheader->i_to_r__)
      MatrixFree(&mri_talheader->i_to_r__);
    if (mri_talheader->r_to_i__)
      MatrixFree(&mri_talheader->r_to_i__);
    
    mri_talheader->i_to_r__ = extract_i_to_r(mri_talheader);
    mri_talheader->r_to_i__ = extract_r_to_i(mri_talheader);
  }
  /////////////////////////////////////////////////////////////////////////////////
  mri_tal = MRIalloc(mri_im->width, mri_im->height, mri_im->depth, mri_im->type);
  MRIcopyHeader(mri_talheader, mri_tal);
  // now fill the talairach volume values
  MRItoTalairachEx(mri_im, mri_tal, lta);

  // binalize the talairach volume (mri_tal)
  MRIbinarize(mri_tal, mri_tal, DEFAULT_DESIRED_WHITE_MATTER_VALUE/2-1, 0, 110) ;

  if (segmentation_fname)
  {
    printf("reading segmented volume %s...\n", segmentation_fname) ;
    mri_seg = MRIread(segmentation_fname) ;
    if (!mri_seg)
      ErrorExit(ERROR_NOFILE, "%s: could not read segmentation from %s",
                Progname, segmentation_fname) ;
    if  (mri_im->linear_transform == 0)
    {
      if (find_cc_seed_with_segmentation(mri_tal, mri_seg, &cc_tal_x, &cc_tal_y, &cc_tal_z) == NO_ERROR)
	cc_seed_set = 1;
    }
  }
  else
    mri_seg = NULL;

  ///////////////////////////////////////////////////////////////////////////
  // find corpus callosum (work under the talairach volume)
  ///////////////////////////////////////////////////////////////////////////
  // cc_seed is given by tal position
  if (cc_seed_set)
  {
    Real xv, yv, zv;
    printf("Using the seed to calculate the cutting plane\n");
    printf("Verify whether the seed point is inside the volume first\n");
    MRItalairachToVoxelEx(mri_im, cc_tal_x, cc_tal_y, cc_tal_z, &xv, &yv, &zv, lta);
    if (xv > (mri_im->width-1) || xv < 0 
	|| yv > (mri_im->height-1) || yv < 0
	|| zv > (mri_im->depth-1) || zv < 0)
    {
      fprintf(stderr, "The seed point (%.2f, %.2f, %.2f) is mapped to a voxel (%.2f, %.2f. %.2f).\n",
	      cc_tal_x, cc_tal_y, cc_tal_z, xv, yv, zv);
      fprintf(stderr, "Make sure that the seed point is given in talaraich position or use -CV option\n");
      return -1;
    }

    mri_cc =
      find_cutting_plane(mri_tal, cc_tal_x, cc_tal_y, cc_tal_z,
                         MRI_SAGITTAL, &x_cc, &y_cc, &z_cc, cc_seed_set, lta) ;
  }
  // cc_seed is give by voxel position
  else if (cc_seed_vol_set) 
  {
    // compute the tal position using lta
    MRIvoxelToTalairachEx(mri_im, cc_vol_x, cc_vol_y, cc_vol_z,
			  &cc_tal_x, &cc_tal_y, &cc_tal_z, lta);
    printf("Calculated talairach cc position (%.2f, %.2f, %.2f)\n",
	   cc_tal_x, cc_tal_y, cc_tal_z);

    // mark as cc_seed_set by the tal
    cc_seed_set = 1;
    printf("Using the seed to calculate the cutting plane\n");
    mri_cc =
      find_cutting_plane(mri_tal, cc_tal_x, cc_tal_y, cc_tal_z,
                         MRI_SAGITTAL, &x_cc, &y_cc, &z_cc, cc_seed_set, lta) ;
  }
  else // seed is not set
  {
    if (find_corpus_callosum(mri_tal,&cc_tal_x,&cc_tal_y,&cc_tal_z, lta) 
        != NO_ERROR)
      mri_cc = NULL ; // could not find corpus_callosum
    else
      mri_cc =
        find_cutting_plane(mri_tal, cc_tal_x, cc_tal_y, cc_tal_z,
                           MRI_SAGITTAL, &x_cc, &y_cc, &z_cc, cc_seed_set, lta) ;
  }

  if (!mri_cc)  /* heuristic failed - use Talairach coordinates */
  {
    cc_tal_x = CORPUS_CALLOSUM_TAL_X ; 
    cc_tal_y = CORPUS_CALLOSUM_TAL_Y; 
    cc_tal_z = CORPUS_CALLOSUM_TAL_Z;
    mri_cc = 
      find_cutting_plane(mri_tal, cc_tal_x, cc_tal_y, cc_tal_z, MRI_SAGITTAL,
                         &x_cc, &y_cc, &z_cc, cc_seed_set, lta) ;
    if (!mri_cc && mri_seg)
    {
      if (find_cc_seed_with_segmentation(mri_tal, mri_seg, &cc_tal_x, &cc_tal_y, &cc_tal_z) == NO_ERROR)
	cc_seed_set = 1;
      mri_cc = 
	find_cutting_plane(mri_tal, cc_tal_x, cc_tal_y, cc_tal_z, MRI_SAGITTAL,
			   &x_cc, &y_cc, &z_cc, cc_seed_set, lta) ;
    }
    
    if (!mri_cc)
      ErrorExit(ERROR_BADPARM, "%s: could not find corpus callosum", Progname);
  }
  // get the src volume version
  // we need the target volume c_(ras) to set the volume correct
  mri_tmp = MRIcopy(mri_im, NULL);   // src volume space
  MRIfromTalairachEx(mri_cc, mri_tmp, lta) ;
  // only 1 or 0 values 
  MRIbinarize(mri_tmp, mri_tmp, 1, 0, 1) ;
  MRIfree(&mri_cc) ; 
  mri_cc = mri_tmp ;  // mri_cc is in the src volume space!

  /* update tal coords of CC based on data (BUG FIX - BRF) */
  // the following is wrong:
  // MRIvoxelToTalairachEx(mri_im, (Real)x_cc, (Real)y_cc, (Real)z_cc,
  // 		      &cc_tal_x, &cc_tal_y, &cc_tal_z, lta) ;

  // find_cutting_plane returns talairach voxel position
  // change them to talairach RAS position
  MRIvoxelToWorld(mri_tal, (Real) x_cc, (Real) y_cc, (Real) z_cc, 
		  &cc_tal_x, &cc_tal_y, &cc_tal_z);
  printf("talairach cc position changed to (%.2f, %.2f, %.2f)\n", cc_tal_x, cc_tal_y, cc_tal_z);

  if (log_fp)
    fprintf(log_fp, "CC:   %d, %d, %d (TAL: %2.1f, %2.1f, %2.1f)\n",
            x_cc, y_cc, z_cc, cc_tal_x, cc_tal_y, cc_tal_z) ;

  ////////////////////////////////////////////////////////////////////////////
  // finding pons location
  ////////////////////////////////////////////////////////////////////////////
  if (pons_seed_vol_set)
  {
    // compute the tal position using lta
    MRIvoxelToTalairachEx(mri_im, pons_vol_x, pons_vol_y, pons_vol_z,
			  &pons_tal_x, &pons_tal_y, &pons_tal_z, lta);
    printf("Using voxel pons seed point, calculated talairach pons position (%.2f, %.2f, %.2f)\n",
	   pons_tal_x, pons_tal_y, pons_tal_z);

    // mark as cc_seed_set by the tal
    pons_seed_set = 1;
  }
  else if (pons_seed_set)
  {
    Real xv, yv, zv;
    printf("Verify whether the seed point is inside the volume first\n");
    MRItalairachToVoxelEx(mri_im, pons_tal_x, pons_tal_y, pons_tal_z, &xv, &yv, &zv, lta);
    if (xv > (mri_im->width-1) || xv < 0 
	|| yv > (mri_im->height-1) || yv < 0
	|| zv > (mri_im->depth-1) || zv < 0)
    {
      fprintf(stderr, "The seed point (%.2f, %.2f, %.2f) is mapped to a voxel (%.2f, %.2f. %.2f).\n",
	      pons_tal_x, pons_tal_y, pons_tal_z, xv, yv, zv);
      fprintf(stderr, "Make sure that the seed point is given in talaraich position or use -CV option\n");
      return -1;
    }
  }
  else // automatic find pons
  {
    MRI  *mri_mask=NULL ;

    if (cc_mask)   /* limit pons to be directly below corpus callosum */
    {
      fprintf(stderr, 
              "masking possible pons locations using cc cutting plane\n") ;
      mri_tmp = MRIcopy(mri_im, NULL) ; // src volume
      mri_mask = MRIcopy(mri_cc, NULL) ; // in src volume space
      MRIdilate(mri_mask, mri_mask) ; MRIdilate(mri_mask, mri_mask) ;
      // change the mask values
      MRIreplaceValues(mri_mask, mri_mask, 0, 255) ; // 0 -> 255
      MRIreplaceValues(mri_mask, mri_mask, 1, 0) ;   // 1 -> 0
      extend_to_lateral_borders(mri_mask, mri_mask, 0) ;
      MRImask(mri_tmp, mri_mask, mri_tmp, 255, 0) ;       // in src volume space
      MRImask(mri_labels, mri_mask, mri_labels, 255, 0); 
      if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
      {
        MRIwrite(mri_tmp, "./erased.mgh") ;  // src volume space
        MRIwrite(mri_mask, "./mask.mgh") ;   // src volume space
      }
    }
    else
      mri_tmp = mri_im ;
    
    MRIfree(&mri_tal) ; 

    // go to talairach space
    mri_tal = MRIalloc(mri_tmp->width, mri_tmp->height, mri_tmp->depth, mri_tmp->type);
    MRIcopyHeader(mri_talheader, mri_tal);
    MRItoTalairachEx(mri_tmp, mri_tal, lta) ;  // erased volume to tal space
    if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
      MRIwrite(mri_tal, "./talponsearch.mgh");

    // binarize
    MRIbinarize(mri_tal, mri_tal, DEFAULT_DESIRED_WHITE_MATTER_VALUE/2-1, 0, 110) ;
    // the last arg is method = 0 (default)
    find_pons(mri_tal, &pons_tal_x, &pons_tal_y, &pons_tal_z,x_cc,y_cc,z_cc,0);
    /////////////////////////////////////////////    // up to here OK    
    if (mri_tmp != mri_im)
    {
      MRIfree(&mri_tmp) ;
      if (mri_mask)
        MRIfree(&mri_mask) ;
    }
  }
  // now use pons_tal_? location 
  mri_pons = 
    find_cutting_plane(mri_tal, pons_tal_x,pons_tal_y, pons_tal_z,
                       MRI_HORIZONTAL, &x_pons, &y_pons, &z_pons,
                       pons_seed_set, lta);

  if (!pons_seed_set && !mri_pons)  /* first attempt failed - try different */
  {
    MRI  *mri_mask=NULL ;

    fprintf(stderr, 
            "initial attempt at finding brainstem failed - initiating backup "
            "plan A....\n") ;
    if (cc_mask)
    {
      mri_tmp = MRIcopy(mri_tal, NULL) ;
      mri_mask = MRIcopy(mri_cc, NULL) ;
      MRIdilate(mri_mask, mri_mask) ; MRIdilate(mri_mask, mri_mask) ;
      MRIreplaceValues(mri_mask, mri_mask, 0, 255) ; // 0-> 255
      MRIreplaceValues(mri_mask, mri_mask, 1, 0) ;   // 1-> 0
      extend_to_lateral_borders(mri_mask, mri_mask, 0) ;
      MRImask(mri_tmp, mri_mask, mri_tmp, 255, 0) ;
      MRImask(mri_labels, mri_mask, mri_labels, 255, 0) ;
      if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
      {
        MRIwrite(mri_tmp, "./planA-erased.mgh") ;
        MRIwrite(mri_mask, "./planA-mask.mgh") ;
      }
    }
    else
      mri_tmp = MRIcopy(mri_tal, NULL);
    MRIfree(&mri_tal) ; // mri_tal = MRItoTalairach(mri_tmp, NULL) ;
    mri_tal = MRIcopy(mri_tmp, NULL);
    MRIbinarize(mri_tal, mri_tal, DEFAULT_DESIRED_WHITE_MATTER_VALUE/2-1, 0, 110) ;
    find_pons(mri_tmp, &pons_tal_x, &pons_tal_y, &pons_tal_z,x_cc,y_cc,z_cc,1);
    if (mri_tmp != mri_tal)
    {
      MRIfree(&mri_tmp) ;
      if (mri_mask)
        MRIfree(&mri_mask) ;
    }
    mri_pons = 
      find_cutting_plane(mri_tal, pons_tal_x,pons_tal_y, pons_tal_z,
                         MRI_HORIZONTAL, &x_pons, &y_pons, &z_pons,
                         pons_seed_set, lta);
  }

  if (!mri_pons) /* heuristic failed to find the pons - try Talairach coords */
  {
    pons_tal_x = PONS_TAL_X ; pons_tal_y = PONS_TAL_Y; pons_tal_z = PONS_TAL_Z;
    mri_pons = 
      find_cutting_plane(mri_im, pons_tal_x,pons_tal_y, pons_tal_z,
                         MRI_HORIZONTAL, &x_pons, &y_pons, &z_pons,
                         pons_seed_set, lta);
    if (!mri_pons)
      ErrorExit(ERROR_BADPARM, "%s: could not find pons", Progname);
  }

  if (log_fp)
  {
    fprintf(log_fp, "PONS: %d, %d, %d (TAL: %2.1f, %2.1f, %2.1f)\n",
            x_pons, y_pons, z_pons, pons_tal_x, pons_tal_y, pons_tal_z) ;
    fclose(log_fp) ;
  }
  mri_tmp2 = MRIcopy(mri_im, NULL); // cutting plane
  if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
    // talairached volume pons cutting plane
    MRIwrite(mri_pons, "./ponstal.mgh");
  MRIfromTalairachEx(mri_pons, mri_tmp2, lta) ;
  MRIbinarize(mri_tmp2, mri_tmp2, 1, 0, 1) ;
  MRIfree(&mri_pons) ; mri_pons = mri_tmp2 ;
  MRIdilate(mri_pons, mri_pons) ;

  MRIfree(&mri_tal) ;

  /* make cuts in both image and labels image to avoid introducing a connection
     with one of the labels
        */
  MRImask(mri_im, mri_cc, mri_im, 1, fill_val) ;
  MRImask(mri_im, mri_pons, mri_im, 1, fill_val) ;
  if (mri_saved_labels)
  {
    MRImask(mri_saved_labels, mri_cc, mri_saved_labels, 1, 0) ;
    MRImask(mri_saved_labels, mri_pons, mri_saved_labels, 1, 0) ;
  }
  if (fill_val)
  {
    fprintf(stderr,"writing out image with cutting planes to 'planes.mgh'.\n");
    MRIwrite(mri_im, "planes.mgh") ;
    fprintf(stderr, "done.\n") ;
    exit(0) ;
  }

  if (mri_saved_labels)
  {
    for (i = 0 ; i < NLABELS ; i++)
      MRIcopyLabel(mri_saved_labels, mri_im, labels[i]) ;
    MRIfree(&mri_saved_labels) ;
  }
  MRIfree(&mri_labels) ; MRIfree(&mri_pons) ; 
  if (!Gdiag)
    fprintf(stderr, "done.\n") ;

  voxsize = findMinSize(mri_im);
  seed_search_size = ceil(SEED_SEARCH_SIZE/voxsize); // in voxel space
  printf("seed_search_size = %d, min_neighbors = %d\n", seed_search_size, MIN_NEIGHBORS);

  // rh side wm
  if (rh_seed_set)
  {
    MRItalairachToVoxelEx(mri_im, rh_tal_x, rh_tal_y,rh_tal_z,&xr,&yr,&zr, lta);
    wm_rh_x = nint(xr) ; wm_rh_y = nint(yr) ; wm_rh_z = nint(zr) ;
  }
  else
  {
    /* find white matter seed point for the right hemisphere */
    // search size is talairach and thus SEED_SEARCH_SIZE should not be changed
    // talairach space is RAS

    MRItalairachToVoxelEx(mri_im, cc_tal_x+2*SEED_SEARCH_SIZE,  
                        cc_tal_y,cc_tal_z,&xr,&yr,&zr, lta);

    printf("search rh wm seed point around talairach space:(%.2f, %.2f, %.2f) SRC: (%.2f, %.2f, %.2f)\n",
	   cc_tal_x+2*SEED_SEARCH_SIZE, cc_tal_y, cc_tal_z, xr, yr, zr);

    wm_rh_x = nint(xr) ; wm_rh_y = nint(yr) ; wm_rh_z = nint(zr) ;

    if ((MRIvox(mri_im, wm_rh_x, wm_rh_y, wm_rh_z) <= WM_MIN_VAL) ||  // dark
        (neighbors_on(mri_im, wm_rh_x, wm_rh_y, wm_rh_z) < MIN_NEIGHBORS)) // one voxel neighbors are dark
    {
      found = xnew = ynew = znew = 0 ;
      min_dist = 10000.0f/voxsize ; // change to voxel distance
      if (Gdiag & DIAG_SHOW)
        fprintf(stderr, "searching for rh wm seed...") ;
      for (z = wm_rh_z-seed_search_size ; z <= wm_rh_z+seed_search_size ; z++)
      {
        zi = mri_im->zi[z] ;
        for (y = wm_rh_y-seed_search_size ; y <= wm_rh_y+seed_search_size ;y++)
        {
          yi = mri_im->yi[y] ;
          for (x = wm_rh_x-seed_search_size ;x <= wm_rh_x+seed_search_size;x++)
          {
            xi = mri_im->xi[x] ;
            if ((MRIvox(mri_im, xi, yi, zi) >= WM_MIN_VAL) &&
                neighbors_on(mri_im, xi, yi, zi) >= MIN_NEIGHBORS)
            {
              found = 1 ;
              xd = (xi - wm_rh_x) ; yd = (yi - wm_rh_y) ; zd = (zi - wm_rh_z) ;
              dist = xd*xd + yd*yd + zd*zd ;
              if (dist < min_dist)
              {
                xnew = xi ; ynew = yi ; znew = zi ;
                min_dist = dist ;
              }
            }
          }
        }
      }
      if (!found)
        ErrorExit(ERROR_BADPARM, 
                  "could not find rh seed point around (%d, %d, %d)",
                  wm_rh_x, wm_rh_y, wm_rh_z) ;
      wm_rh_x = xnew ; wm_rh_y = ynew ; wm_rh_z = znew ; 
      if (Gdiag & DIAG_SHOW)
        fprintf(stderr, "found at (%d, %d, %d)\n", xnew, ynew, znew) ;
    }
    if (Gdiag & DIAG_SHOW)
      fprintf(stderr, "rh seed point at (%d, %d, %d): %d neighbors on.\n",
              wm_rh_x, wm_rh_y, wm_rh_z, 
              neighbors_on(mri_im, wm_rh_x, wm_rh_y, wm_rh_z)) ;

  }

  // lh side wm
  if (lh_seed_set)
  {
    MRItalairachToVoxelEx(mri_im, lh_tal_x, lh_tal_y,lh_tal_z,&xr,&yr,&zr, lta);
    wm_lh_x = nint(xr) ; wm_lh_y = nint(yr) ; wm_lh_z = nint(zr) ;
  }
  else
  {
    /* find white matter seed point for the left hemisphere */
    MRItalairachToVoxelEx(mri_im, cc_tal_x-2*SEED_SEARCH_SIZE,  
                        cc_tal_y, cc_tal_z, &xr, &yr, &zr, lta);
    printf("search lh wm seed point around talairach space (%.2f, %.2f, %.2f), SRC: (%.2f, %.2f, %.2f)\n",
	   cc_tal_x-2*SEED_SEARCH_SIZE, cc_tal_y, cc_tal_z, xr, yr, zr);

    wm_lh_x = nint(xr) ; wm_lh_y = nint(yr) ; wm_lh_z = nint(zr) ;
    if ((MRIvox(mri_im, wm_lh_x, wm_lh_y, wm_lh_z) <= WM_MIN_VAL) ||
      (neighbors_on(mri_im, wm_lh_x, wm_lh_y, wm_lh_z) < MIN_NEIGHBORS))
    {
      found = xnew = ynew = znew = 0 ;
      min_dist = 10000.0f/voxsize ;
      if (Gdiag & DIAG_SHOW)
        fprintf(stderr, "searching for lh wm seed...") ;
      for (z = wm_lh_z-seed_search_size ; z <= wm_lh_z+seed_search_size ; z++)
      {
        zi = mri_im->zi[z] ;
        for (y = wm_lh_y-seed_search_size ; y <= wm_lh_y+seed_search_size ;y++)
        {
          yi = mri_im->yi[y] ;
          for (x = wm_lh_x-seed_search_size ;x <= wm_lh_x+seed_search_size;x++)
          {
            xi = mri_im->xi[x] ;
            if ((MRIvox(mri_im, xi, yi, zi) >= WM_MIN_VAL) &&
                (neighbors_on(mri_im, xi, yi, zi) >= MIN_NEIGHBORS))
            {
              found = 1 ;
              xd = (xi - wm_lh_x) ; yd = (yi - wm_lh_y) ; zd = (zi - wm_lh_z) ;
              dist = xd*xd + yd*yd + zd*zd ;
              if (dist < min_dist)
              {
                xnew = xi ; ynew = yi ; znew = zi ;
                min_dist = dist ;
              }
            }
          }
        }
      }
      if (!found)
        ErrorExit(ERROR_BADPARM, 
                  "could not find lh seed point around (%d, %d, %d)",
                  wm_lh_x, wm_lh_y, wm_lh_z) ;
      if (Gdiag & DIAG_SHOW)
        fprintf(stderr, "found at (%d, %d, %d)\n", xnew, ynew, znew) ;
      wm_lh_x = xnew ; wm_lh_y = ynew ; wm_lh_z = znew ; 

    }
    if (Gdiag & DIAG_SHOW)
      fprintf(stderr, "lh seed point at (%d, %d, %d): %d neighbors on.\n",
              wm_lh_x, wm_lh_y, wm_lh_z, 
              neighbors_on(mri_im, wm_lh_x, wm_lh_y, wm_lh_z)) ;
  }

  if (segmentation_fname)
  {
    MRIeraseTalairachPlaneNewEx(mri_seg, mri_cc, MRI_SAGITTAL, x_cc, y_cc, z_cc, 
				mri_seg->width, fill_val, lta);
    edit_segmentation(mri_im, mri_seg) ;
    MRIeraseTalairachPlaneNewEx(mri_im, mri_cc, MRI_SAGITTAL, x_cc, y_cc, z_cc, 
				mri_seg->width, fill_val, lta);
  }

  MRIfree(&mri_cc) ;
  mri_lh_fill = MRIclone(mri_im, NULL) ;
  mri_rh_fill = MRIclone(mri_im, NULL) ;
  mri_lh_im = MRIcopy(mri_im, NULL) ;
  mri_rh_im = MRIcopy(mri_im, NULL) ;

  if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
    MRIwrite(mri_im, "fill0.mgh") ;
  fprintf(stderr, "filling left hemisphere...\n") ;
  MRIfillVolume(mri_lh_fill, mri_lh_im, wm_lh_x, wm_lh_y, wm_lh_z,lh_fill_val);
  fprintf(stderr, "filling right hemisphere...\n") ;
  MRIfillVolume(mri_rh_fill, mri_rh_im, wm_rh_x, wm_rh_y, wm_rh_z,rh_fill_val);
  MRIfree(&mri_lh_im) ; MRIfree(&mri_rh_im) ; MRIfree(&mri_im) ;

  /* find and eliminate degenerate surface locations caused by diagonal
     connectivity in which a vertex is simultaneously on both sides of 
     the surface. This might cause bubbles in the volume - seems unlikely,
     but....
  */
  fprintf(stderr, "filling degenerate left hemisphere surface locations...\n");
  MRIfillDegenerateLocations(mri_lh_fill, lh_fill_val) ;
  fprintf(stderr,"filling degenerate right hemisphere surface locations...\n");
  MRIfillDegenerateLocations(mri_rh_fill, rh_fill_val) ;

  /*  must redo filling to avoid holes caused by  filling of  degenerate  locations */
  mri_lh_im =  MRIcopy(mri_lh_fill, NULL);
  mri_rh_im =  MRIcopy(mri_rh_fill, NULL);
  fprintf(stderr, "refilling left hemisphere...\n") ;
  MRIfillVolume(mri_lh_fill, mri_lh_im, wm_lh_x, wm_lh_y, wm_lh_z,lh_fill_val);
  fprintf(stderr, "refilling right hemisphere...\n") ;
  MRIfillVolume(mri_rh_fill, mri_rh_im, wm_rh_x, wm_rh_y, wm_rh_z,rh_fill_val);
  MRIfree(&mri_lh_im) ; MRIfree(&mri_rh_im) ; 

  fprintf(stderr, "combining hemispheres...\n") ;
  MRIvoxelToTalairachEx(mri_lh_fill, (Real)wm_lh_x, (Real)wm_lh_y, (Real)wm_lh_z,
                      &xr, &yr, &zr, lta) ;
  MRIvoxelToTalairachEx(mri_rh_fill, (Real)wm_rh_x, (Real)wm_rh_y, (Real)wm_rh_z,
                      &xr, &yr, &zr, lta) ;
  
  mri_fill = MRIcombineHemispheres(mri_lh_fill, mri_rh_fill, NULL, 
                                   wm_lh_x, wm_lh_y, wm_lh_z,
                                   wm_rh_x, wm_rh_y, wm_rh_z) ;

  if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
  {
    MRIwrite(mri_lh_fill, "./lh_fill.mgh"); 
    MRIwrite(mri_rh_fill, "./rh_fill.mgh");
  }

  MRIfree(&mri_lh_fill) ; MRIfree(&mri_rh_fill) ;

  if (atlas_name)
  {
    MRI *mri_p_ventricle, *mri_fg, *mri_bg, *mri_T1, *mri_ventricle ;
    char subjects_dir[STRLEN], mri_dir[STRLEN], *cp ;

    cp = getenv("MRI_DIR") ;
    if (!cp)
      ErrorExit(ERROR_BADPARM, "%s: could not read MRI_DIR from environment",
                Progname) ;
    strcpy(mri_dir, cp) ;

    cp = getenv("SUBJECTS_DIR") ;
    if (!cp)
      ErrorExit(ERROR_BADPARM, 
                "%s: could not read SUBJECTS_DIR from environment",Progname) ;
    strcpy(subjects_dir, cp) ;


    sprintf(fname, "%s/%s/mri/transforms/%s.m3d", subjects_dir,sname,
            atlas_name) ;
    m3d = MRI3DreadSmall(fname) ;
    if (!m3d)
      ErrorExit(ERROR_NOFILE, 
                "%s: could not open transform file %s\n",
                Progname, fname) ;
    fprintf(stderr, "filling ventricles using atlas %s...\n", atlas_name) ;
    mri_p_ventricle = 
      mriReadBinaryProbabilities(atlas_name, "left_ventricle",m3d,sname,NULL);
    if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
      MRIwrite(mri_p_ventricle, "p_left_ventricle.mgh") ;

    sprintf(fname, "%s/%s/mri/T1", subjects_dir,sname) ;
    mri_T1 = MRIread(fname) ;
    if (!mri_T1)
      ErrorExit(ERROR_NOFILE, "%s: could not read T1 volume %s\n", 
                Progname, fname) ;
#if 0
#define TMP_FILL_VAL   5
    MRIprobabilityThreshold(mri_fill, mri_p_ventricle, mri_fill, 50.0, 
                            TMP_FILL_VAL) ;
    MRIdilateLabel(mri_fill, mri_fill, TMP_FILL_VAL, 2) ;
    MRIreplaceValues(mri_fill, mri_fill, TMP_FILL_VAL, lh_fill_val) ;
#else
    MRItalairachToVoxelEx(mri_fill,cc_tal_x-10, cc_tal_y, cc_tal_z, &xr,&yr,&zr, lta);
    mri_ventricle = 
      MRIfillVentricle(mri_fill, mri_p_ventricle, mri_T1, NULL, 90.0f, 
                     lh_fill_val, nint(xr), mri_fill->width-1) ;
    if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
      MRIwrite(mri_ventricle, "left_ventricle.mgh") ;
    MRIdilate(mri_ventricle, mri_ventricle) ;
    MRIdilate(mri_ventricle, mri_ventricle) ;
    MRIunion(mri_fill, mri_ventricle, mri_fill) ;
    MRIfree(&mri_ventricle) ;
#endif
                            
    MRIfree(&mri_p_ventricle) ;
    
    mri_p_ventricle = 
      mriReadBinaryProbabilities(atlas_name,"right_ventricle",m3d,sname,NULL);
    if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
      MRIwrite(mri_p_ventricle, "p_right_ventricle.mgh") ;
#if 0
    MRIprobabilityThreshold(mri_fill, mri_p_ventricle, mri_fill, 50.0, 
                            TMP_FILL_VAL);
    MRIdilateLabel(mri_fill, mri_fill, TMP_FILL_VAL, 2) ;
    MRIreplaceValues(mri_fill, mri_fill, TMP_FILL_VAL, rh_fill_val) ;
#else
    MRItalairachToVoxelEx(mri_fill,cc_tal_x+10,cc_tal_y, cc_tal_z, &xr,&yr,&zr, lta);
    mri_ventricle = 
      MRIfillVentricle(mri_fill, mri_p_ventricle, mri_T1, NULL, 90.0f, 
                       rh_fill_val, 0, nint(xr)) ;
    MRIwrite(mri_ventricle, "right_ventricle.mgh") ;
    MRIdilate(mri_ventricle, mri_ventricle) ;
    MRIdilate(mri_ventricle, mri_ventricle) ;
    MRIunion(mri_fill, mri_ventricle, mri_fill) ;
#endif

    MRIfree(&mri_T1) ; MRIfree(&mri_ventricle) ; MRIfree(&mri_p_ventricle) ;
    MRI3DmorphFree(&m3d) ;

    mri_fg = MRIclone(mri_fill, NULL) ;
    MRIvox(mri_fg, wm_rh_x, wm_rh_y, wm_rh_z) = rh_fill_val ;
    MRIvox(mri_fg, wm_lh_x, wm_lh_y, wm_lh_z) = lh_fill_val ;
    MRIgrowLabel(mri_fill, mri_fg, lh_fill_val, lh_fill_val) ;
    MRIgrowLabel(mri_fill, mri_fg, rh_fill_val, rh_fill_val) ;
    mri_bg = MRIclone(mri_fill, NULL) ;
    MRIvox(mri_bg, 0, 0, 0) = 1 ;
    MRIgrowLabel(mri_fill, mri_bg, 0, 1) ;
    MRIturnOnFG(mri_fill, mri_fg, mri_bg) ;
    MRIturnOffBG(mri_fill, mri_bg) ;
  }
  
  fprintf(stderr, "writing output to %s...\n", out_fname) ;
  MRIwrite(mri_fill, out_fname) ;
  msec = TimerStop(&then) ;
  fprintf(stderr,"filling took %2.1f minutes\n", (float)msec/(60*1000.0f));

  if (lta)
  {
    int totRH, totLH;
    totRH = 0;
    totLH = 0;
    verifyLRSplit(mri_fill, lta, cc_tal_x, &badRH, &badLH, &totRH, &totLH);

    if ((badRH > 10000) || (badLH > 10000))
    {
      if (lta)
	LTAfree(&lta);
      MRIfree(&mri_fill);
      MRIfree(&mri_talheader);

      fprintf(stderr, "badRH = %d/%d, badLH=%d/%d\n", badRH, totRH, badLH, totLH);
      errno = 0; // otherwise it will print standard error
      ErrorExit(ERROR_BADPARM, "Please check filled volume.  Cerebellum may be included.\n");
    }
  }
  if (lta)
    LTAfree(&lta);
  MRIfree(&mri_fill);
  MRIfree(&mri_talheader);

  return(0) ;
}

static int 
fill_brain(MRI *mri_fill, MRI *mri_im, int threshold)
{
  int dir = -1, nfilled = 10000, ntotal = 0,iter = 0;
  int im0,im1,j0,j1,i0,i1,imnr,i,j;
  int v1,v2,v3,vmax ;

  mriFindBoundingBox(mri_im) ;
  while (nfilled>min_filled && iter<MAX_ITERATIONS)
  {
    iter++;
    nfilled = 0;
    dir = -dir;
    if (dir==1)   /* filling foreground */
    {
      im0=1;
      im1=mri_fill->depth-1;
      i0=j0=1;
      i1=mri_fill->height ; j1=mri_fill->width ;
    } 
    else          /* filling background */
    {
      im0=mri_fill->depth-2;
      im1= -1;
      i0 = mri_fill->height - 2 ; j0 = mri_fill->width - 2 ;
      i1=j1= -1;
    }
    for (imnr=im0;imnr!=im1;imnr+=dir)
    {
      for (i=i0;i!=i1;i+=dir)
      {
        for (j=j0;j!=j1;j+=dir)
        {
          if (MRIvox(mri_fill, j, i, imnr) ==0)   /* not filled yet */
          {
            if ((threshold<0 &&   /* previous filled off */
                 MRIvox(mri_im, j, i, imnr)<-threshold)  ||  
                (threshold>=0 &&
                 MRIvox(mri_im, j, i, imnr) >threshold))   /* wm is on */
            {
              /* three inside 6-connected nbrs */
              v1=MRIvox(mri_fill, j, i, imnr-dir);
              v2=MRIvox(mri_fill, j, i-dir, imnr);
              v3=MRIvox(mri_fill, j-dir, i, imnr) ;
              if (v1>0||v2>0||v3>0)       /* if any are on */
              {

                /* set vmax to biggest of three interior neighbors */
                vmax = (v1>=v2&&v1>=v3)?v1:((v2>=v1&&v2>=v3)?v2:v3);

                MRIvox(mri_fill, j, i, imnr) = vmax;
                nfilled++;
                ntotal++;
              }
            }
          }
        }
      }
    }
    if (Gdiag & DIAG_SHOW && DIAG_VERBOSE_ON)
      fprintf(stderr, "%d voxels filled\n",nfilled);
  } 
  fprintf(stderr, "total of %d voxels filled...",ntotal);
  if (Gdiag & DIAG_SHOW)
    fprintf(stderr, "\n") ;
  return(NO_ERROR) ;
}

static int
fill_holes(MRI *mri_fill)
{
  int  nfilled, ntotal = 0, cnt, cntmax= neighbor_threshold ;
  int im0,x0,i0,z,i,x;
  int v,vmax;

  do
  {
    nfilled = 0;
    for (z=1;z!=mri_fill->depth-1;z++)
    for (i=1;i!=mri_fill->height-1;i++)
    for (x=1;x!=mri_fill->width-1;x++)
    if (MRIvox(mri_fill, x, i, z)==0 && 
        i>ylim0-10 && i<ylim1+10 && x>xlim0-10 && x<xlim1+10)
    {
      cnt = 0;
      vmax = 0;
      for (im0= -1;im0<=1;im0++)
      for (i0= -1;i0<=1;i0++)
      for (x0= -1;x0<=1;x0++)
      {
        v = MRIvox(mri_fill, x+x0, i+i0, z+im0) ;
        if (v>vmax) vmax = v;
        if (v == 0) cnt++;              /* count # of nbrs which are off */
        if (cnt>cntmax) im0=i0=x0=1;    /* break out of all 3 loops */
      }
      if (cnt<=cntmax)   /* toggle pixel (off to on, or on to off) */
      {
        MRIvox(mri_fill, x, i, z) = vmax; 
        nfilled++;
        ntotal++;
      }
    }
    if (Gdiag & DIAG_SHOW && DIAG_VERBOSE_ON)
      fprintf(stderr, "%d holes filled\n",nfilled);
  } while (nfilled > 0) ;
  
  if (Gdiag & DIAG_SHOW)
    fprintf(stderr, "total of %d holes filled\n",ntotal);
  return(NO_ERROR) ;
}
/*----------------------------------------------------------------------
            Parameters:

           Description:
----------------------------------------------------------------------*/
static int
get_option(int argc, char *argv[])
{
  int  nargs = 0 ;
  char *option ;
  
  option = argv[1] + 1 ;            /* past '-' */
  if (!strcmp(option, "-help"))
    print_help() ;
  else if (!strcmp(option, "-version"))
    print_version() ;
  else if (!strcmp(option, "rval"))
  {
    rh_fill_val = atoi(argv[2]) ;
    nargs = 1 ;
    fprintf(stderr,"using %d as fill val for right hemisphere.\n",
            rh_fill_val);
  }
  else if (!strcmp(option, "lval"))
  {
    lh_fill_val = atoi(argv[2]) ;
    nargs = 1 ;
    fprintf(stderr,"using %d as fill val for left hemisphere.\n",lh_fill_val);
  }
  else if (!strcmp(option, "debug_voxel"))
  {
    Gx = atoi(argv[2]) ;
    Gy = atoi(argv[3]) ;
    Gz = atoi(argv[4]) ;
    nargs = 3 ;
    printf("debugging voxel (%d,  %d,  %d)\n", Gx, Gy, Gz)  ;
  }
  else if (!strcmp(option, "lh"))
  {
    lh_seed_set = 1 ;
    lh_tal_x = atof(argv[2]) ;
    lh_tal_y = atof(argv[3]) ;
    lh_tal_z = atof(argv[4]) ;
    fprintf(stderr, "using Talairach position (%2.1f, %2.1f, %2.1f) as lh seed\n",
            lh_tal_x, lh_tal_y, lh_tal_z) ;
    nargs = 3 ;
  }
  else if (!strcmp(option, "segmentation"))
  {
    segmentation_fname = argv[2] ;
    fprintf(stderr, "using segmentation %s...\n", segmentation_fname) ;
    nargs = 1 ;
  }
  else if (!strcmp(option, "rh"))
  {
    rh_seed_set = 1 ;
    rh_tal_x = atof(argv[2]) ;
    rh_tal_y = atof(argv[3]) ;
    rh_tal_z = atof(argv[4]) ;
    fprintf(stderr, "using Talairach position (%2.1f, %2.1f, %2.1f) as rh seed\n",
            rh_tal_x, rh_tal_y, rh_tal_z) ;
    nargs = 3 ;
  }
  else if (!strcmp(option, "ccmask"))
  {
    cc_mask = !cc_mask ;
    fprintf(stderr,"%susing corpus callosum to mask possible location of "
            "pons.\n", cc_mask ? "" : "not ");
  }
  else if (!strcmp(option, "atlas"))
  {
    atlas_name = argv[2] ; sname = argv[3] ;
    nargs = 2 ;
    fprintf(stderr, "using atlas %s for auto filling...\n", atlas_name) ;
  }
  else if (!strcmp(option,"xform"))
  {
    struct stat stat_buf;
    if (stat(argv[2], &stat_buf) < 0)
      ErrorExit(ERROR_BADPARM,"ERROR: cound not stat xform:%s.  Does it exist?\n", argv[2]);
    lta = LTAreadEx(argv[2]);
    if (lta==0)
      ErrorExit(ERROR_BADPARM,"ERROR: cound not load lta from %s.\n", argv[2]);      
    fprintf(stderr, "INFO: Using %s and its offset for Talairach volume ...\n", argv[2]);
    nargs = 1; // consumed 1 args
  }
  else if (!strcmp(option, "PV"))
  {
    pons_vol_x = atof(argv[2]) ;
    pons_vol_y = atof(argv[3]) ;
    pons_vol_z = atof(argv[4]) ;
    nargs = 3 ;
    fprintf(stderr, "using voxel position (%2.0f, %2.0f, %2.0f) as pons seed point\n",
            pons_vol_x, pons_vol_y, pons_vol_z) ;
    pons_seed_vol_set = 1 ;
  }
  else if (!strcmp(option, "CV"))
  {
    cc_vol_x = atof(argv[2]) ;
    cc_vol_y = atof(argv[3]) ;
    cc_vol_z = atof(argv[4]) ;
    nargs = 3 ;
    fprintf(stderr, "using voxel position (%2.0f, %2.0f, %2.0f) as corpus callosum seed point\n",
            cc_vol_x, cc_vol_y, cc_vol_z) ;
    cc_seed_vol_set = 1 ;
  }
  else switch (toupper(*option))
  {
  case 'P':
    pons_tal_x = atof(argv[2]) ;
    pons_tal_y = atof(argv[3]) ;
    pons_tal_z = atof(argv[4]) ;
    nargs = 3 ;
    fprintf(stderr, "using Talairach position (%2.0f, %2.0f, %2.0f) as pons seed point\n",
            pons_tal_x, pons_tal_y, pons_tal_z) ;
    pons_seed_set = 1 ;
    break ;
  case 'C':
    cc_tal_x = atof(argv[2]) ;
    cc_tal_y = atof(argv[3]) ;
    cc_tal_z = atof(argv[4]) ;
    nargs = 3 ;
    fprintf(stderr, "using Talairach position (%2.0f, %2.0f, %2.0f) as corpus callosum seed point\n",
            cc_tal_x, cc_tal_y, cc_tal_z) ;
    cc_seed_set = 1 ;
    break ;
  case 'T':
    if (sscanf(argv[2], "%d", &neighbor_threshold) < 1)
    {
      fprintf(stderr, "fill: could not scan threshold from '%s'", argv[2]) ;
      exit(1) ;
    }
    nargs = 1 ;
    break ;
  case 'L':
    log_fp = fopen(argv[2], "w") ;
    if (!log_fp)
      ErrorExit(ERROR_BADPARM, "%s: could not open cutting plane log file %s",
                Progname, argv[2]) ;
    nargs = 1 ;
    fprintf(stderr, "logging cutting plane coordinates to %s...\n", argv[2]) ;
    break ;
  case 'F':
    fill_val = atoi(argv[2]) ;
    /*    min_filled = atoi(argv[2]) ;*/
    nargs = 1 ;
    break ;
  case 'D':
    logging = 1 ;
    break ;
  case '?':
  case 'U':
    print_help() ;
    exit(1) ;
    break ;
  default:
    fprintf(stderr, "unknown option %s\n", argv[1]) ;
    exit(1) ;
    break ;
  }

  return(nargs) ;
}
static void
print_version(void)
{
  fprintf(stderr, "fill version %s\n", vcid) ;
  exit(0) ;
}
static void
print_help(void)
{
  fprintf(stderr, 
          "usage: %s [options] <input MR directory> <output MR directory>\n",
          Progname) ;
  fprintf(stderr, "where options are:\n") ;
  fprintf(stderr, 
          "\t-T <threshold> - specify fill_holes threshold (default=%d)\n",
          DEFAULT_NEIGHBOR_THRESHOLD) ;
  fprintf(stderr,
	  "\t-xform <xformname> - use xform dst offset to get an accurate Talairach volume\n");
  fprintf(stderr,
	  "Seed point options in Talairach coordinates:\n");
  fprintf(stderr, 
          "\t-C <x> <y> <z> - the Talairach coords of the seed for the corpus callosum\n") ;
  fprintf(stderr, 
          "\t-P <x> <y> <z> - the Talairach coords of the seed for the pons\n");
  fprintf(stderr, 
          "\t-lh <x> <y> <z> - the Talairach coords of the white matter seed\n"
          "\t                 for the left hemisphere\n") ;
  fprintf(stderr, 
          "\t-rh <x> <y> <z> - the Talairach coords of the white matter seed\n"
          "\t                 for the right hemisphere\n") ;
  fprintf(stderr, 
	  "Alternative seed point options in voxel coordinates:\n");
  fprintf(stderr, 
          "\t-CV <x> <y> <z> - the voxel coords of the seed for the corpus callosum\n") ;
  fprintf(stderr, 
          "\t-PV <x> <y> <z> - the voxel coords of the seed for the pons\n");
  fprintf(stderr,
          "\n");
  exit(0) ;
}

/* build a set of slices in Talairach space, and find the one in which 
   the central connected component has the smallest cross-sectional
   area. Then use this as a mask for erasing ('cutting') stuff out
   of the input image.
*/

#define MAX_SLICES        15  /* 41*/
#define HALF_SLICES       ((MAX_SLICES-1)/2)
#define CUT_WIDTH         1
#define HALF_CUT          ((CUT_WIDTH-1)/2)
#define SEARCH_STEP       3
#define MAX_OFFSET        50

/* aspect ratios are dy/dx */
#define MIN_CC_AREA       350  /* smallest I've seen is 389 */
#define MAX_CC_AREA      1400  /* biggest I've seen is 1154 */
#define MIN_CC_ASPECT     0.1
#define MAX_CC_ASPECT     0.75

#define MIN_PONS_AREA     350  /* smallest I've seen is 385 */
#define MAX_PONS_AREA     790  // 700  /* biggest I've seen is 620 */  
#define MIN_PONS_ASPECT   0.6  /* was .8 */
#define MAX_PONS_ASPECT   1.2

static MRI *
find_cutting_plane(MRI *mri_tal, Real x_tal, Real y_tal,Real z_tal,int orientation,
                   int *pxv, int *pyv, int *pzv, int seed_set, const LTA *lta)
{
  // here is the limitation on the voxsize being up to .5 mm
  MRI        *mri_slices[MAX_SLICES*2], *mri_filled[MAX_SLICES*2], *mri_cut=NULL, *mri_cut_vol ;
  Real       dx, dy, dz, x, y, z, aspect,MIN_ASPECT,MAX_ASPECT,
             aspects[MAX_SLICES*2] ;
  int        slice, offset, area[MAX_SLICES*2], min_area0, min_slice,xo,yo,found,
             xv, yv, zv, x0, y0, z0, xi, yi, zi, MIN_AREA, MAX_AREA, done, where ;
  FILE       *fp = NULL ;   /* for logging pons and cc statistics */
  char       fname[STRLEN], *name ;
  MRI_REGION region ;
  double    voxsize;
  int min_area, max_area, max_slices;
  int half_slices, cut_width, half_cut, search_step, max_offset;
  int slice_size;
  int xoo=0;
  int yoo=0;

  voxsize = findMinSize(mri_tal);

  // mri coming is a talairached volume
  /* search for valid seed point */
  if (mri_tal->linear_transform || lta)
  {
    MRIworldToVoxel(mri_tal, x_tal, y_tal,  z_tal, &x, &y, &z) ;
    // talairach volume voxel position
    xv = nint(x) ; yv = nint(y) ; zv = nint(z) ;
  }
  else  // if no talairach, we don't know where to look
  {
    MRIboundingBox(mri_tal, 1, &region) ;
    xv = region.x + region.dx/2; yv = region.y + region.dy/2; zv = region.z + region.dz/2;
  }

  if (MRIvox(mri_tal, xv, yv, zv) < WM_MIN_VAL)  /* seed not on?? Try and fix it */
  {
    printf("find_cutting_plane:seed point not in structure! Searching neighborhood...\n") ;
    MRIfindNearestNonzeroLocation(mri_tal, 9, xv, yv, zv, &xv, &yv, &zv) ;
  }
  // (xv, yv, zv) is the tal volume index position

  min_slice = -1 ;
  // note that the following assumes that the volume is in coronal orientation
  /////////////////////////////////////////////////////////////////////////////
  switch (orientation)
  {
  default:
    // used to find the corpus callosum
  case MRI_SAGITTAL:             // using the sagittal cutting plane
    dx = 1.0 ; dy = dz = 0.0 ; 
    name = "corpus callosum" ;
    MIN_AREA = MIN_CC_AREA;  MAX_AREA = MAX_CC_AREA;
    MIN_ASPECT = MIN_CC_ASPECT ; MAX_ASPECT = MAX_CC_ASPECT ;
    if (logging)
      fp = fopen(CC_LOG_FILE, "a") ;
    where = xv ; yo = yv ; xo = zv ; // sagital z-y plane regarded as x-y plane 
    break ;
    // used to find the pons
  case MRI_HORIZONTAL:           //           horizontal cutting plane
    dz = 1.0 ; dx = dy = 0.0 ; 
    name = "pons" ;
    MIN_AREA = MIN_PONS_AREA ; MAX_AREA = MAX_PONS_AREA ;
    MIN_ASPECT = MIN_PONS_ASPECT ; MAX_ASPECT = MAX_PONS_ASPECT ;
    if (logging)
      fp = fopen(PONS_LOG_FILE, "a") ;
    where = yv ; yo = zv ; xo = xv ;// horizontal z-x plane regarded as y-x plane
    break ;
  case MRI_CORONAL:              //          coronal cutting plane
    dy = 1.0 ; dx = dz = 0.0 ; 
    MIN_AREA = MIN_CC_AREA ; MAX_AREA = MAX_CC_AREA ;
    MIN_ASPECT = MIN_CC_ASPECT ; MAX_ASPECT = MAX_CC_ASPECT ;
    name = "coronal" ;
    where = zv ; yo = yv ; xo = xv ;
    break ;
  }
  ////////////////////////////////////////////////////////////////
  // convert consts to the voxel count 
  /////////////////////////////////////////////////////////////////
  min_area = floor(MIN_AREA/(voxsize*voxsize));
  if (min_area <= 0)
    min_area = 1;
  max_area = ceil(MAX_AREA/(voxsize*voxsize));
  max_slices = ceil(MAX_SLICES/voxsize);
  half_slices = (max_slices-1)/2;
  cut_width = ceil(CUT_WIDTH/voxsize);
  half_cut = (cut_width-1)/2;
  search_step = ceil(SEARCH_STEP/voxsize);
  max_offset = ceil(MAX_OFFSET/voxsize);
  /////////////////////////////////////////////////////////////////////
  fprintf(stderr, "Looking for area (min, max) = (%d, %d)\n", min_area, max_area);
  /////////////////////////////////////////////////////////////////////
  // mri is talairached volume
  mri_slices[0] = MRIextractPlane(mri_tal, NULL, orientation, where);
  // get the blob in the slice
  mri_filled[0] = MRIfillFG(mri_slices[0],NULL,xo,yo,0,WM_MIN_VAL,127,&area[0]);

  //  region is given in terms of voxel counts, not real size
  MRIboundingBox(mri_filled[0], 1, &region) ;

#if 0
#undef DIAG_VERBOSE_ON
#define DIAG_VERBOSE_ON  1
#endif
  
  if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
  {
    // this works fine
    sprintf(fname, "cutting_plane_%s_slice0.mgh", orientation == MRI_SAGITTAL ? "cc":"pons");
    MRIwrite(mri_slices[0], fname) ;
    sprintf(fname, "cutting_plane_%s_filled0.mgh", orientation==MRI_SAGITTAL?"cc":"pons");
    MRIwrite(mri_filled[0], fname) ;
  }
  
  /* now check to see if it could be a valid seed point based on:
     1) bound on the area
     2) the connected component is completely contained in the slice.
     */
  slice_size = mri_filled[0]->width;
  aspect = (Real)region.dy / (Real)region.dx ;
  done =  
    ((area[0] >= min_area) &&
     (area[0] <= max_area) &&
     (aspect  >= MIN_ASPECT) &&
     (aspect  <= MAX_ASPECT) &&
     (region.y > 0) &&
     (region.x > 0) &&
     (region.x+region.dx < slice_size -1) &&  // SLICE_SIZE-1) &&
     (region.y+region.dy < slice_size -1));    // SLICE_SIZE-1)) ;
  fprintf(stderr, "area[0] = %d (min = %d, max = %d), aspect = %.2f (min = %.2f, max = %.2f)\n", 
	  area[0], min_area, max_area, aspect, MIN_ASPECT, MAX_ASPECT);
  fprintf(stderr, done ? "no need to search\n" : "need search nearby\n");

  // corpus callosum zy plane is regarded as xy plane
  // pons            xz plane is regarded as xy plane

  //////////////////////////////// seed set option //////////////////////////////////////////////
  if (seed_set)  /* just fill around the specified seed */
  {
    done = 1 ;
    MRIboundingBox(mri_filled[0], 1, &region) ;
    // looking for corpus callosum
    if (orientation == MRI_SAGITTAL)/* extend to top and bottom of slice */ 
    {
      region.dy = mri_filled[0]->height - region.y; // SLICE_SIZE - region.y ;
    }
    // find the voxel position
    MRIworldToVoxel(mri_tal, x_tal, y_tal,  z_tal, &x, &y, &z) ;
    // xv,yv,zv are modified (*pxv, *pxy, *pxz are cached value)
    xv = *pxv = nint(x) ; yv = *pyv = nint(y) ; zv = *pzv = nint(z) ;
    fprintf(stderr, "using seed (%d, %d, %d), TAL = (%2.1f, %2.1f, %2.1f)\n",
            *pxv, *pyv, *pzv, x_tal, y_tal, z_tal) ;
    mri_cut = MRIcopy(mri_filled[0], NULL) ;

    // find the non-zero value location and paint with 1
    for (xv = region.x ; xv < region.x+region.dx ; xv++)
    {
      found = 0 ;
      for (yv = region.y ; yv < region.y+region.dy ; yv++)
      {
        if (!found)
          found  = (MRIvox(mri_cut, xv, yv, 0) > 0) ;

        if (found)
          MRIvox(mri_cut, xv, yv, 0) = 1 ;
      }
    }
    // initialize (xv, yv, zv) again from the cached value
    xv = *pxv ; yv = *pyv ; zv = *pzv ; //talairach voxel index position
    if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
    {
      sprintf(fname, "%s_filled.mgh", 
              orientation == MRI_SAGITTAL ? "cc":"pons");
      MRIwrite(mri_slices[0], fname) ;
      sprintf(fname, "%s_cut.mgh", 
              orientation == MRI_SAGITTAL ? "cc":"pons");
      MRIwrite(mri_cut, fname) ;
    }
  }
  ////////////////////////////// end of seed set option ////////////////////////////

  // z-y plane is xy plane for corpus callosum 
  // x-z plane is xy plane for pons

  ////////////////////// seed not set option case //////////////////////////////////
  /// note this is done only when done is set //////////////////////////////////////
  if (!seed_set && done)  /* center the seed */
  {
    find_slice_center(mri_filled[0], &xi, &yi) ; // get the slice voxel index of the center
    // so far xo, yo are the values initialized at the orientation switch at the beginning
    // horizontal  xo = xv, yo = zv  -> xv = xi, zv = yi  why not just do this way?????
    // sagittal    xo = zv, yo = yv  -> zv = xi, yv = yi
    switch (orientation)   
    {
    default:
    case MRI_HORIZONTAL: xv += xi - xo ; zv += yi - yo ; break ; // yv remain the same
    case MRI_SAGITTAL:   zv += xi - xo ; yv += yi - yo ; break ; // xv remain the same
    }
    x = (Real)xv ; y = (Real)yv ; z = (Real)zv ;
    MRIvoxelToWorld(mri_tal, x, y, z, &x_tal, &y_tal, &z_tal) ;
    // got the new slice center position in talairach space
    // instead of original talairach space position
  }
  //////////////////// end of seed set and done case ////////////////////////////////

  MRIfree(&mri_slices[0]) ;
  MRIfree(&mri_filled[0]) ;

  offset = 0 ;
  ///////////////////// seed not set and not done yet ///////////////////////////////
  // now search the region for seed point which satisfy done condition
  if (!seed_set) while (!done)
  {
    offset += search_step ;   /* search at a greater radius */
    if (offset >= max_offset)
      return(NULL) ;
    fprintf(stderr, "using +/- offset search region where offset is %d.....\n", offset);
    // looking around 3 dimensional area
    // note that loop contain xv, yv
    for (z0 = zv-offset ; !done && z0 <= zv+offset ; z0 += offset)
    {
      zi = mri_tal->zi[z0] ; // safe way of getting the value 
      for (y0 = yv-offset ; !done && y0 <= yv+offset ; y0 += offset)
      {
        yi = mri_tal->yi[y0] ; 
        for (x0 = xv-offset ; !done && x0 <= xv+offset ; x0 += offset)
        {
          xi = mri_tal->xi[x0] ; 
          switch (orientation)
          {
          default:
          case MRI_HORIZONTAL: where = yi ; xoo = xi; yoo = zi; break ; 
	    // looking for pons (xz) plane
          case MRI_SAGITTAL:   where = xi ; xoo = zi; yoo = yi; break ; 
	    // looking for corpus callosum (zy) plane
          case MRI_CORONAL:    where = zi ; break ;
          }
          mri_slices[0] = MRIextractPlane(mri_tal, NULL, orientation, where);
	  ////////////////////////////////////////////////////////////////////////////////
	  // slice location (where) is different but the same point in the (xy) plane
	  // filled at the same point (xo,yo) given at the beginning
	  // the original way only one loop is enough for each orientation
          // mri_filled[0] = MRIfillFG(mri_slices[0], NULL, xo, yo, 0,WM_MIN_VAL,127, &area[0]);
          mri_filled[0] = MRIfillFG(mri_slices[0], NULL, xoo, yoo, 0,WM_MIN_VAL,127, &area[0]);
          MRIboundingBox(mri_filled[0], 1, &region) ;

          if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
          {
            sprintf(fname, "%s_seed.mgh", 
                    orientation == MRI_SAGITTAL ? "cc":"pons");
            MRIwrite(mri_slices[0], fname) ;
            sprintf(fname, "%s_seed_fill.mgh", 
                    orientation == MRI_SAGITTAL ? "cc":"pons");
            MRIwrite(mri_filled[0], fname) ;
          }
    
          /* now check to see if it could be a valid seed point based on:
             1) bound on the area
             2) the connected component is completely contained in the slice.
             */

          aspect = (Real)region.dy / (Real)region.dx ;

	  // fprintf(stderr, "area[0] = %d (min = %d, max = %d), aspect = %.2f (min = %.2f, max = %.2f)\n", 
	  //  area[0], min_area, max_area, aspect, MIN_ASPECT, MAX_ASPECT);

	  // does this  area satisfy the condition?
          if ((area[0] >= min_area) &&
              (area[0] <= max_area) &&
              (aspect  >= MIN_ASPECT) &&
              (aspect  <= MAX_ASPECT) &&
              (region.y > 0) &&
              (region.x > 0) &&
              (region.x+region.dx < slice_size-1) && //SLICE_SIZE-1) &&
              (region.y+region.dy < slice_size-1))   //SLICE_SIZE-1))
          {
	    fprintf(stderr, "area[0] = %d (min = %d, max = %d), aspect = %.2f (min = %.2f, max = %.2f)\n", 
		    area[0], min_area, max_area, aspect, MIN_ASPECT, MAX_ASPECT);

            /* center the seed */
	    // xv and yv get modified, but done = 1 is set and thus ok.
            find_slice_center(mri_filled[0], &xv, &yv) ; // center index in the plane 
	    // getting blob center
            switch (orientation)   
            {
            default:
            case MRI_HORIZONTAL: /*xi += xv - xo ; zi += yv - yo*/ xi = xv; zi = yv ; break ; // yi remains (slice value)
            case MRI_SAGITTAL:  /* zi += xv - xo ; yi += yv - yo*/ zi = xv; yi = yv ; break ; // xi remains (slice value)
            }

            x = (Real)xi ; y = (Real)yi ; z = (Real)zi ;
	    // get the talairach RAS position
            MRIvoxelToWorld(mri_tal, x, y, z, &x_tal, &y_tal, &z_tal) ;
            done = 1 ;
            xv = xi ; yv = yi ; zv = zi ;
          }
          MRIfree(&mri_slices[0]) ;
          MRIfree(&mri_filled[0]) ;
        }
      }
    }
  }
  if (Gdiag & DIAG_SHOW)
  {
    // Real  xnv, ynv, znv ;

    MRIvoxelToWorld(mri_tal, xv, yv, zv, &x, &y, &z) ;
    // cannot use the following without knowing the src volume
    // MRItalairachVoxelToVoxelEx(mri_tal, xv, yv, zv, &xnv, &ynv, &znv, lta) ;
    fprintf(stderr, 
          "%s seed point found at Tal vol: (%d, %d, %d) TAL: (%.2f, %.2f, %.2f)\n", 
          name, xv, yv, zv, x, y, z);
  }
  // note that x_tal, y_tal, z_tal are updated by finding the seed point
  // found the seed point look around the slice below and above
  if (!seed_set)   /* find slice with smallest cross-section */
  {
    for (slice = 0 ; slice < max_slices ; slice++)
    {
      offset = slice - half_slices ;
      x = x_tal + dx*offset ; y = y_tal + dy*offset ; z = z_tal + dz*offset ;
      MRIworldToVoxel(mri_tal, x, y,  z, &x, &y,&z) ;
      xv = nint(x) ; yv = nint(y) ; zv = nint(z) ;
      switch (orientation)
      {
      default:
      case MRI_HORIZONTAL: where = yv ; break ;
      case MRI_SAGITTAL:   where = xv ; break ;
      case MRI_CORONAL:    where = zv ; break ;
      }
      mri_slices[slice] = MRIextractPlane(mri_tal,NULL,orientation, where);
      mri_filled[slice] = MRIfillFG(mri_slices[slice],NULL,xo,yo,0,WM_MIN_VAL,127,&area[slice]);
      MRIboundingBox(mri_filled[slice], 1, &region) ;
      aspects[slice] = (Real)region.dy / (Real)region.dx ;

#if 0      
      /* don't trust slices that extend to the border of the image */
      if (!region.x || !region.y || region.x+region.dx >= SLICE_SIZE-1 ||
          region.y+region.dy >= SLICE_SIZE-1)
        area[slice] = 0 ;
#endif
      
      if (Gdiag & DIAG_SHOW && DIAG_VERBOSE_ON)
        fprintf(stderr, "cutting_plane_slice[%d] @ (%d, %d, %d): area = %d\n", 
                slice, xv, yv, zv, area[slice]) ;
      
      if (orientation == MRI_SAGITTAL)/* extend to top and bottom of slice */
        region.dy = slice_size - region.y; //  SLICE_SIZE - region.y ;
      
      /*    for (yv = region.y ; yv < region.y+region.dy ; yv++)*/
      for (xv = region.x ; xv < region.x+region.dx ; xv++)
      {
        found = 0 ;
        for (yv = region.y ; yv < region.y+region.dy ; yv++)
        {
          if (!found)
            found  = (MRIvox(mri_filled[slice], xv, yv, 0) > 0) ;

          if (found)
            MRIvox(mri_filled[slice], xv, yv, 0) = 1 ;
        }
      }
      
      if ((Gdiag & DIAG_WRITE) && !(slice % 1) && DIAG_VERBOSE_ON)
      {
        sprintf(fname, "cutting_plane_%s_slice%d.mgh", 
                orientation == MRI_SAGITTAL ? "cc":"pons", slice);
        MRIwrite(mri_slices[slice], fname) ;
        sprintf(fname, "cutting_plane_%s_filled%d.mgh", 
                orientation == MRI_SAGITTAL ? "cc":"pons", slice);
        MRIwrite(mri_filled[slice], fname) ;
      }
    }
    // finding pons
    if (orientation == MRI_HORIZONTAL)
    {
      min_area0 = floor(10000./(voxsize*voxsize)) ;
      min_slice = -1 ;
      for (slice = 1 ; slice < max_slices-1 ; slice++)
      {
	fprintf(stderr, "area[%d]=%d, min_area0 = %d, max_area = %d\n",
		slice, area[slice], min_area0, max_area);
	// find smallest area which is between min_area and max_area
	if (area[slice] < min_area0 && 
            (area[slice] >= min_area && area[slice] <= max_area)) 
        {
          min_area0 = area[slice] ;
          min_slice = slice ;
        }
	// find the min slice
	//if (area[slice] < min_area0)
	//  {
	//    min_area0 = area[slice];
	//    min_slice = slice;
	//  }
      }
    }
    else    /* search for middle of corpus callosum */
    {
      int valid[255]; // MAX_SLICES] /*, num_on, max_num_on, max_on_slice_start*/ ;
      
      for (slice = 1 ; slice < max_slices-1 ; slice++)
      {
        valid[slice] =
          ((area[slice]    >= min_area)   && (area[slice]    <= max_area) &&
           (aspects[slice] >= MIN_ASPECT) && (aspects[slice] <= MAX_ASPECT));
      }

#if 1
      min_area0 = mri_slices[1]->width*mri_slices[1]->height*mri_slices[1]->depth;
      for (slice = 1 ; slice < max_slices-1 ; slice++)
      {
        float slice_area ;
        if (!valid[slice])
          continue ;
        slice_area = MRItotalVoxelsOn(mri_slices[slice], WM_MIN_VAL) ;
	fprintf(stderr, "slice=%d, slice_area=%.1f, min_area0 = %d, max_area = %d\n",
		slice, slice_area, min_area0, max_area);
        if (slice_area < min_area0)
        {
          min_area0 = slice_area ;
          min_slice = slice ;
        }
      }
#else    
      max_on_slice_start = max_num_on = num_on = 0 ;
      for (slice = 1 ; slice < max_slices-1 ; slice++)
      {
        if (valid[slice])
          num_on++ ;
        else
        {
          if (num_on > max_num_on)
          {
            max_num_on = num_on ;
            max_on_slice_start = slice - num_on ;
          }
        }
      }
      if (num_on > max_num_on)   /* last slice was on */
      {
        max_num_on = num_on ;
        max_on_slice_start = slice - num_on ;
      }
      min_slice = max_on_slice_start + (max_num_on-1)/2 ;
      min_area0 = area[min_slice] ;
#endif      
    } // end of corpus callosum
    ////////////////////////////////////////////////////////
    fprintf(stderr, "min_slice = %d, min_area = %d\n", 
	    min_slice, min_area0);

    if (min_slice < 0)
      return(NULL) ;

    if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
    {
      sprintf(fname, "cutting_plane_%s_minslice.mgh", 
              orientation == MRI_SAGITTAL ? "cc":"pons");
      MRIwrite(mri_slices[min_slice], fname) ;
      sprintf(fname, "cutting_plane_%s_minfilled.mgh", 
              orientation == MRI_SAGITTAL ? "cc":"pons");
      MRIwrite(mri_filled[min_slice], fname) ;
    }

    if (logging)
    {
      fprintf(fp, "area %d, aspect %2.2f\n", 
              area[min_slice],aspects[min_slice]);
      fclose(fp) ;
    }
    
    offset = min_slice - half_slices ;
    x = x_tal + dx*offset ; y = y_tal + dy*offset ; z = z_tal + dz*offset ; 
    MRIworldToVoxel(mri_tal, x, y,  z, &x, &y, &z) ;
    *pxv = nint(x) ; *pyv = nint(y) ; *pzv = nint(z);
    // copy min slice filled plane
    mri_cut = MRIcopy(mri_filled[min_slice], NULL) ;
    if (Gdiag & DIAG_SHOW)
      fprintf(stderr, 
              "%s: cutting at slice %d, area %d, (%d, %d, %d)\n", 
              name, min_slice, min_area0, *pxv, *pyv, *pzv) ;

    for (slice = 0 ; slice < max_slices ; slice++)
    {
      MRIfree(&mri_slices[slice]) ;
      MRIfree(&mri_filled[slice]) ;
    }
  }
  else   /* seed specified by user */
  {
  }
  // copy original volume
  mri_cut_vol = MRIclone(mri_tal, NULL) ;
  switch (orientation)
  {
  default:
  case MRI_HORIZONTAL: where = *pyv ; break ;
  case MRI_SAGITTAL:   where = *pxv ; break ;
  case MRI_CORONAL:    where = *pzv ; break ;
  }
  MRIfillPlane(mri_cut, mri_cut_vol, orientation, where, 200) ;
  MRIfree(&mri_cut) ;
  return(mri_cut_vol) ;
}

static int
find_slice_center(MRI *mri,  int *pxo, int *pyo)
{
  int  x, y, dist, min_total_dist, total_dist, width, height, x1, y1, xo, yo,
       border, xi, yi ;

  xo = yo = 0 ;
  width = mri->width ; height = mri->height ;
  min_total_dist = -1 ;
  for (y = 0 ; y < height ; y++)
  {
    for (x = 0 ; x < width ; x++)
    {
      /* find total distance to all other points */
      if (MRIvox(mri, x, y, 0) >= WM_MIN_VAL)
      {
	// given a point (x,y) whose grey >= MIN_VAL
	// calculate the distance to all points whose grey >= MIN_VAL
        total_dist = 0 ;
        for (y1 = 0 ; y1 < height ; y1++)
        {
          for (x1 = 0 ; x1 < width ; x1++)
          {
            if (MRIvox(mri, x1, y1, 0) >= WM_MIN_VAL) 
            {
              dist = (x1-x)*(x1-x) + (y1-y)*(y1-y) ;
              total_dist += dist ;
            }
          }
        }
	// find the smallest point
        if (min_total_dist < 0 || total_dist < min_total_dist)
        {
          min_total_dist = total_dist ;
          xo = x ; yo = y ;
        }
      }
    }
  }
  // (x0, y0) gives the total distance minimum.
  /* now find a point which is not on the border */
  border = 1 ;
  for (y = yo-1 ; border && y <= yo+1 ; y++)
  {
    if (y < 0 || y >= height)
      continue ;
    for (x = xo-1 ; border && x <= xo+1 ; x++)
    {
      if (x < 0 || x >= width)
        continue ;
      if (MRIvox(mri, x, y, 0) >= WM_MIN_VAL) /* see if it is a border pixel */
      {
        border = 0 ;
        for (y1 = y-1 ; y1 <= y+1 ; y1++)
        {
          yi = mri->yi[y1] ;
          for (x1 = x-1 ; x1 <= x+1 ; x1++)
          {
            xi = mri->xi[x1] ;
            if (!MRIvox(mri, xi, yi, 0))
              border = 1 ;
          }
        }
      }
      if (!border)  /* (x,y) is not a border pixel */
      {
        xo = x ; yo = y ;
      }
    }
  }

  *pxo = xo ; *pyo = yo ;
  return(NO_ERROR) ;
}

#define CC_SPREAD       17
#define MIN_THICKNESS   3
#define MAX_THICKNESS   20

///////////////////////////////////////////////////////////////////////////////////
// assumes the coronal orientation.  
// i.e. x axis is along LR axis and y axis is along IS axis
static int
find_corpus_callosum(MRI *mri_tal, Real *pccx, Real *pccy, Real *pccz, const LTA *lta)
{
  int         xv, yv, zv, max_y, thickness, y1, xcc, ycc, x, y,x0 ;
  Real        xr, yr, zr ;
  MRI_REGION  region ;
  int cc_spread, min_thickness, max_thickness, slice_size;
  double voxsize=findMinSize(mri_tal);
  
  cc_spread = ceil(CC_SPREAD/voxsize);
  min_thickness = ceil(MIN_THICKNESS/voxsize); // estimate bigger
  max_thickness = ceil(MAX_THICKNESS/voxsize);
  slice_size = mri_tal->width;

  MRIboundingBox(mri_tal, 1, &region) ;
  // bounding box center position in voxel coords
  x0 = region.x+region.dx/2 ;

  // this function is called with mri being talairached volume
  // get the talairach coords (0,0,0) in the voxel space
  if (mri_tal->linear_transform || lta)
  {
    MRIworldToVoxel(mri_tal, 0.0, 0.0, 0.0, &xr, &yr, &zr);   /* everything is now in tal coords */
    xv = nint(xr) ; yv = nint(yr) ; zv = nint(zr) ;
  }
  else
  {
    xv = x0; yv = region.y+region.dy/2; zv = region.z+region.dz/2; 
  }
  /* find the column with the lowest starting y value of any sign. thick. */
  xcc = ycc = max_y = 0 ; 
  for (x = x0-cc_spread ; x <= x0+cc_spread ; x++)
  {
    /* search for first non-zero pixel */
    // in the talairach origin coronal slice from the top
    for (y = 0 ; y < slice_size ; y++)
    {
      if (MRIvox(mri_tal, x, y, zv) >= WM_MIN_VAL)  
        break ;
    }
    // find y which is greater than WM_MIN_VAL
    /* check to make sure it as reasonably thick */
    if ((y < slice_size) && (y > max_y)) // within the region and bigger than so far
    {
      for (y1 = y, thickness = 0 ; y1 < slice_size ; y1++, thickness++)
        if (!MRIvox(mri_tal, x, y1, zv)) // if becomes zero, then break out
          break ;
      // found the zero voxel at y1 -> thinckness
      if ((thickness > min_thickness) && (thickness < max_thickness))
      {
        xcc = x ; ycc = y+thickness/2 ;  /* in middle of cc */
        max_y = y ;            // mark starting y position
        if (Gdiag & DIAG_SHOW && DIAG_VERBOSE_ON)
          fprintf(stderr, "potential cc found at (%d, %d), thickness = %d\n",
                  xcc, ycc, thickness) ;
      }
    }
  }

  if (!max_y)
    return(ERROR_BADPARM) ;

  /* now convert the in-plane coords to Talairach coods */
  MRIvoxelToWorld(mri_tal, xcc, ycc, zv, pccx, pccy, pccz) ;

  find_cc_slice(mri_tal, pccx, pccy, pccz, lta) ;

  return(NO_ERROR) ;
}

#define MIN_BRAINSTEM_THICKNESS         15
#define MAX_BRAINSTEM_THICKNESS         35
#define MIN_DELTA_THICKNESS             6
#define MIN_BRAINSTEM_HEIGHT            25

/* if can't find a really thick piece of brainstem (15 voxels wide),
   then try to find some contiguous slices that are fairly wide.
*/
#define MIN_CONT_BRAINSTEM_THICKNESS    8
#define MIN_CONT_BRAINSTEM_HEIGHT       3

static int
find_pons(MRI *mri, Real *p_ponsx, Real *p_ponsy, Real *p_ponsz, 
          int x_cc, int y_cc, int z_cc, int method)
{
  MRI   *mri_slice, *mri_filled, *mri_closed ;
  Real  xr, yr, zr ;
  int   xv, yv, zv, x, y, width, height, thickness, xstart, area,xmax,
        bheight ;
  MRI_REGION region ;
  int max_brainstem_thickness, min_brainstem_thickness;
  int min_brainstem_height, min_delta_thickness;
  int min_cont_brainstem_thickness, min_cont_brainstem_height;
  double voxsize = findMinSize(mri);

  max_brainstem_thickness = ceil(MAX_BRAINSTEM_THICKNESS/voxsize);
  min_brainstem_thickness = floor(MIN_BRAINSTEM_THICKNESS/voxsize);
  min_brainstem_height = floor(MIN_BRAINSTEM_HEIGHT/voxsize);
  min_delta_thickness = floor(MIN_DELTA_THICKNESS/voxsize);
  min_cont_brainstem_thickness = floor(MIN_CONT_BRAINSTEM_THICKNESS/voxsize);
  min_cont_brainstem_height = floor(MIN_CONT_BRAINSTEM_HEIGHT/voxsize);
  //
  fprintf(stderr, "brainstem (min, max) thickness = (%d,%d)\n", 
	  min_brainstem_thickness, max_brainstem_thickness);
  fprintf(stderr, "brainstem min height = %d\n", min_brainstem_height);
  fprintf(stderr, "min_delta_thickness = %d\n", min_delta_thickness);
  fprintf(stderr, "min_cont_brainstem_thickness = %d\n", 
	  min_cont_brainstem_thickness);
  fprintf(stderr, "min_cont_brainstem_height = %d\n",
	  min_cont_brainstem_height);
  ///////////////////////////////////////////////////
  thickness = xstart = -1 ;
  
  MRIboundingBox(mri, 10, &region) ;
  xr = (Real)(region.x+region.dx/2) ;
  yr = (Real)(region.y+region.dy/2) ;
  zr = (Real)(region.z+region.dz/2) ;

  xv = (int)xr ; yv = (int)yr ; zv = (int)zr ;
  // completely ignore the above
  xv = x_cc ; yv = y_cc ; zv = z_cc ;  /* use corpus callosum!! */

  // talairach space now
  MRIvoxelToWorld(mri, xr, yr, zr, &xr, &yr, &zr);

  // get sagittal slice
  mri_slice = MRIextractPlane(mri, NULL, MRI_SAGITTAL,xv) ;


  /* (xv,yv,zv) are the coordinates of the center of the slice */

  if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
    MRIwrite(mri_slice, "brainstem_slice.mgh") ;

  // first time method = 0 (default)
  switch (method)
  {
  case 1:
    mri_closed = MRIclose(mri_slice, NULL) ;  /* remove small holes */
    if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
      MRIwrite(mri_closed, "brainstem_closed.mgh") ;

/*
  search from the front of the head at the bottom of a sagittal slice
  for the first set of contiguous slices that are significantly thick, 
  this will (hopefully) be the brainstem. 
*/
    
    /* first find the brainstem */
    width = mri_slice->width ; height = mri_slice->height ;
    bheight = 0 ;
    for (y = height-1 ; y >= 0 ; y--)
    {
      for (x = width-1 ; x >= 0 ; x--)
      {
        thickness = 0 ;
        while (MRIvox(mri_closed, x, y, 0) && x >= 0)
        {
          if (!thickness)
            xstart = x ;
          thickness++ ; x-- ;
        }
        if (thickness >= min_cont_brainstem_thickness && 
            thickness <= max_brainstem_thickness)
          break ;
        else
          thickness = 0 ;
      }
      if (thickness > 0)   /* found the slice */
        bheight++ ;
      else
        bheight = 0 ;
      if (bheight >= min_cont_brainstem_height)
        break ;  /* found it */
    }
    MRIfree(&mri_closed) ;
    break ;
    ////////////////////// default here
  default:
  case 0:
/*
  search from the front of the head at the bottom of a sagittal slice
  for the first significantly thick slice of white matter, which will
  be the brainstem. Then, follow the contour of the brainstem upwards
  until the first 'backwards' indentation which is about where we
  want to make the cut.
*/
    
    /* first find the brainstem */
    width = mri_slice->width ; height = mri_slice->height ;
    // start from the bottom
    for (y = height-1 ; y >= 0 ; y--)
    {
      // from face to back
      for (x = width-1 ; x >= 0 ; x--)
      {
        thickness = 0 ;
	// grey scale non-zero and 
        while (MRIvox(mri_slice, x, y, 0) && x >= 0)
        {
          if (!thickness) // if thickness = 0, then
            xstart = x ;  // mark sthe position
          thickness++ ; x-- ;
        }
	// if in-between min and max braistem thickness
        if (thickness >= min_brainstem_thickness && 
            thickness <= max_brainstem_thickness)
          break ; // break
        else
          thickness = 0 ;
      }
      if (thickness > 0)   /* found the slice */
        break ;
    }
    break ;
  }

  /*
    Then, follow the contour of the brainstem upwards until the first 
    'backwards' indentation which is about where we want to make the cut.
  */

  mri_filled = MRIfillFG(mri_slice, NULL,
			 xstart-(thickness-1)/2,y,0,WM_MIN_VAL,127,&area);

  if (Gdiag & DIAG_SHOW && DIAG_VERBOSE_ON)
    fprintf(stderr, "found brainstem at y=%d, x = (%d --> %d), area=%d\n",
            y, xstart, xstart-thickness+1, area) ;

  if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
    MRIwrite(mri_filled, "brainstem_filled.mgh") ;
  // working up to here
/* 
   now, starting from that slice, proceed upwards until we find a
   'cleft', which is about where we want to cut.
*/
  xmax = x = xstart ;  /* needed for initialization */
  bheight = 0 ;
  do
  {
    xstart = x ;
    y-- ; bheight++ ;
    if (y < 0)
      ErrorReturn(ERROR_BADPARM, 
                  (ERROR_BADPARM, "could not find bottom of brainstem")) ;
    for (x = width-1 ; !MRIvox(mri_filled,x,y,0) && (x >= 0) ; x--)
    {}

    if (x < 0)/* sometimes pons is disconnected at cleft: assume prior slice */
    {
      y++ ; bheight-- ;
      for (x = width-1 ; !MRIvox(mri_filled,x,y,0) && (x >= 0) ; x--)
      {}
      break ;
    }

    if (Gdiag & DIAG_SHOW && DIAG_VERBOSE_ON)
      fprintf(stderr, "slice %d, xstart %d\n", y, x) ;
    if (x > xmax)
    {
      /* check to see if we've gone too far */
      if ((bheight > min_brainstem_height) && (x-xmax >= min_delta_thickness))
      {
        y-- ;   /* gone too far */
        break ;
      }
      xmax = x ;
    }
  } while ((xmax - x < min_delta_thickness) && (y >0)) ;

  /* now search forward to find center of slice */
  for (thickness = 0, xstart = x ; 
       (x >= 0) && 
       (thickness < max_brainstem_thickness) &&
       MRIvox(mri_slice, x, y, 0) ; 
       x--)
    thickness++ ;
  x = xstart - (thickness-1)/2 ;
  // we were working on the sagittal slice i.e. x -> z, y-> y
  zv = x ; yv = y ;  /* convert to voxel coordinates */
  xr = (Real)xv ; yr = (Real)yv ; zr = (Real)zv ;
  MRIvoxelToWorld(mri, xr, yr, zr, p_ponsx, p_ponsy, p_ponsz) ;
  if (Gdiag & DIAG_SHOW)
    fprintf(stderr, 
	    "putative pons found at %d, %d -> (%d, %d, %d) TAL=(%.2f, %.2f, %.2f)\n", 
            xstart-thickness/2, y, xv, yv, zv, *p_ponsx, *p_ponsy, *p_ponsz) ;

  MRIfree(&mri_slice) ;
  MRIfree(&mri_filled) ;
  return(NO_ERROR) ;
}

/*
  NOTE: mri is already in talairach coords.
*/
static int
find_cc_slice(MRI *mri_tal, Real *pccx, Real *pccy, Real *pccz, const LTA *lta)
{
  // here we can handle only up to .5 mm voxel size
  int         area[MAX_SLICES*2], min_area, min_slice, slice, offset,xv,yv,zv,
              xo, yo ;
  MRI         *mri_slice, *mri_filled ;
  Real        aspect, x_tal, y_tal, z_tal, x, y, z, xvv, yvv, zvv;
  MRI_REGION  region ;
  char        fname[STRLEN] ;
  int half_slices;
  double voxsize = findMinSize(mri_tal);
  int slice_size = mri_tal->width;
  int max_slices = ceil(MAX_SLICES/voxsize);
  int max_cc_area = ceil(MAX_CC_AREA/(voxsize*voxsize));
  int min_cc_area = floor(MIN_CC_AREA/(voxsize*voxsize));

  half_slices = floor(HALF_SLICES/voxsize);
  if ( half_slices <= 0)
    half_slices = 1;

  x_tal = *pccx ; y_tal = *pccy ; z_tal = *pccz ;
  offset = 0 ;
  xo = yo = (slice_size-1)/2 ;  /* center point of the slice */
  for (slice = 0 ; slice < max_slices ; slice++)
  {
    offset = slice - half_slices ;
    x = x_tal + offset ; y = y_tal ; z = z_tal ;
    MRIworldToVoxel(mri_tal, x, y,  z, &x, &y, &z) ;
    xv = nint(x) ; yv = nint(y) ; zv = nint(z) ;
    mri_slice = MRIextractPlane(mri_tal, NULL, MRI_SAGITTAL, xv);
    mri_filled =  MRIfillFG(mri_slice, NULL, zv, yv,0,WM_MIN_VAL,127,&area[slice]);
    MRIboundingBox(mri_filled, 1, &region) ;
    aspect = (Real)region.dy / (Real)region.dx ;

    /* don't trust slices that extend to the border of the image */
    if (!region.x || !region.y || region.x+region.dx >= slice_size -1 ||
        region.y+region.dy >= slice_size-1)
      area[slice] = 0 ;
    
    if (Gdiag & DIAG_SHOW && DIAG_VERBOSE_ON)
      fprintf(stderr, "slice[%d] @ (%d, %d, %d): area = %d\n", 
              slice, xv, yv, zv, area[slice]) ;

    if ((Gdiag & DIAG_WRITE) && !(slice % 1) && DIAG_VERBOSE_ON)
    {
      sprintf(fname, "cc_slice%d.mgh", slice);
      MRIwrite(mri_slice, fname) ;
      sprintf(fname, "cc_filled%d.mgh", slice);
      MRIwrite(mri_filled, fname) ;
    }
    MRIfree(&mri_filled) ; MRIfree(&mri_slice) ;
  }
  
  min_area = 10000 ; min_slice = -1 ;
  for (slice = 1 ; slice < max_slices-1 ; slice++)
  {
    if (area[slice] < min_area && 
        (area[slice] >= min_cc_area && area[slice] <= max_cc_area))
    {
      min_area = area[slice] ;
      min_slice = slice ;
    }
  }
  
  /* couldn't find a good slice - don't update estimate */
  if (min_slice < 0)
    ErrorReturn(ERROR_BADPARM, 
                (ERROR_BADPARM, "%s: could not find valid seed for the cc",
                 Progname));
  
  offset = min_slice - half_slices ;
  *pccx = x = x_tal + offset ; *pccy = y = y_tal ; *pccz = z = z_tal ;
  
  // just for debugging
  MRIworldToVoxel(mri_tal, x, y,  z, &xvv, &yvv, &zvv) ;
  if (Gdiag & DIAG_SHOW)
  {
    // Real xv, yv, zv ;
    // you cannot call this function without knowing the src volume 
    // MRItalairachVoxelToVoxelEx(mri_tal, x, y, z, &xv, &yv, &zv, lta) ;
    // fprintf(stderr, 
    //         "updating initial cc seed to (%d, %d, %d)\n", 
    //         nint(xv), nint(yv), nint(zv)) ;
    fprintf(stderr, "updating initial cc seed to Tal vol (%.2f, %.2f, %.2f) TAL (%.2f, %.2f, %.2f)\n",
	    xvv, yvv, zvv, x, y, z);
  }
  return(NO_ERROR) ;
}

static int
neighbors_on(MRI *mri, int x0, int y0, int z0)
{
  int   nbrs = 0 ;

  if (MRIvox(mri,x0-1,y0,z0) >= WM_MIN_VAL)  
    nbrs++ ;
  if (MRIvox(mri,x0+1,y0,z0) >= WM_MIN_VAL)
    nbrs++ ;
  if (MRIvox(mri,x0,y0+1,z0) >= WM_MIN_VAL)
    nbrs++ ;
  if (MRIvox(mri,x0,y0-1,z0) >= WM_MIN_VAL)
    nbrs++ ;
  if (MRIvox(mri,x0,y0,z0+1) >= WM_MIN_VAL)
    nbrs++ ;
  if (MRIvox(mri,x0,y0,z0-1) >= WM_MIN_VAL)
    nbrs++ ;
  return(nbrs) ;
}
#if 0
static int mriExtendMaskDownward(MRI *mri) ;
static int
mriExtendMaskDownward(MRI *mri)
{
  int     width, height, depth, x, y, z, y1 ;
  BUFTYPE *psrc, val ;

  width = mri->width ;
  height = mri->height ;
  depth = mri->depth ;

  for (z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      psrc = &MRIvox(mri, 0, y, z) ;
      for (x = 0 ; x < width ; x++)
      {
        if ((val = *psrc++) > 0)
        {
          for (y1 = y ; y1 < mri->height ; y1++)
            MRIvox(mri, x, y1, z) = val ;
        }
      }
    }
  }
  return(NO_ERROR) ;
}

#endif

/*
  take the volume in mri_im and find the 'on' connected component
  containing the point (x_seed, y_seed, z_seed), then fill interior holds
  and remove exterior islands to ensure that it contains one connected
  mass (this will be one hemisphere).
*/

static int
MRIfillVolume(MRI *mri_fill, MRI *mri_im, int x_seed, int y_seed, int z_seed,
              int fill_val)
{
  int   x, y, z ;

  MRIvox(mri_fill, x_seed, y_seed, z_seed) = fill_val ;
  if (!Gdiag)
    fprintf(stderr, "filling volume: pass 1 of 3...") ;
  /* fill from 2 seeds in wm outwards */
  fill_brain(mri_fill, mri_im, WM_MIN_VAL);

  /* Set im to initial outward fill */
  MRIcopy(mri_fill, mri_im) ;
  MRIclear(mri_fill) ;
  MRIvox(mri_fill,1,1,1) = 255;              /* seed for background */

  /* fill in connected component of background */  
  if (!Gdiag)
    fprintf(stderr, "\rfilling volume: pass 2 of 3...") ;
  fill_brain(mri_fill, mri_im, -WM_MIN_VAL);

  if (fill_holes_flag)              
    fill_holes(mri_fill);   /* fill in islands with fewer than 10 nbrs on */
  
  /* put complement into im (im==on means part of background) */
  MRIcopy(mri_fill, mri_im) ;
  MRIclear(mri_fill) ;

  MRIvox(mri_fill, x_seed, y_seed, z_seed) = fill_val ;

  /* fill in background of complement (also sometimes called the foreground) */
  if (!Gdiag)
    fprintf(stderr, "\rfilling volume: pass 3 of 3...") ;
  fill_brain(mri_fill, mri_im, -WM_MIN_VAL);   

  mriFindBoundingBox(mri_fill) ;
  
  if (fill_holes_flag)
    fill_holes(mri_fill);

  if (!Gdiag)
    fprintf(stderr, "done.\n") ;
  for (z = 0 ; z < mri_fill->depth ; z++)
    for (y = 0; y < mri_fill->height ; y++)
      for (x = 0; x < mri_fill->width ; x++)
      {
        if (z==0||z==mri_fill->depth-1) 
          MRIvox(mri_fill, x, y, z) = 0;
      }
  return(NO_ERROR) ;
}

/*
  NOTE: this procedure sets some global variable (xlim0, ylim0, xlim1, ylim1)
  which are used in a couple of places!!!! Sorry, but don't blame me -
  it was from Anders' and Marty's original code....
*/
static int
mriFindBoundingBox(MRI *mri_im)
{
  int  x, y, z ;

  /* find bounding box for valid data */
  ylim0 = mri_im->height ; xlim0 = mri_im->width ;
  ylim1 = xlim1 = 0;
  for (z = 0 ; z < mri_im->depth ; z++)
  {
    for (y = 0 ; y < mri_im->height; y++)
    {
      for (x = 0 ; x < mri_im->width ; x++)
      {
        if (MRIvox(mri_im, x, y, z) >= WM_MIN_VAL)
        {
          if (y<ylim0) ylim0=y;
          if (y>ylim1) ylim1=y;
          if (x<xlim0) xlim0=x;
          if (x>xlim1) xlim1=x;
        }
      }
    }
  }
  return(NO_ERROR) ;
}

/*
  combine the two filled hemispheres into a single filled volume.
  Overlap in the volumes is resolved by filling from the most lateral
  part of each hemisphere, and making the fill variable so that wide 
  connections are filled before narrow ones (defined by the # of neighbors
  that are on). Note that this routine modifies mri_lh and mri_rh so
  that they do not overlap.
*/

MRI *
MRIcombineHemispheres(MRI *mri_lh, MRI *mri_rh, MRI *mri_dst,
                      int wm_lh_x, int wm_lh_y, int wm_lh_z,
                      int wm_rh_x, int wm_rh_y, int wm_rh_z)
{
  int     width, height, depth, x, y, z, v, ncorrected, ambiguous, lh, rh,
          nfilled, xi, yi, zi, xk, yk, zk, nleft, nright, nambiguous, i ;
  BUFTYPE *plh, *prh, *pdst, vlh, vrh ;

  width = mri_lh->width ;
  height = mri_lh->height ;
  depth = mri_lh->depth ;

  if (!mri_dst)
    mri_dst = MRIclone(mri_lh, NULL) ;

  ambiguous = rh = lh = 0 ;
  for (ncorrected = z = 0 ; z < depth && !ambiguous ; z++)
  {
    for (y = 0 ; y < height && !ambiguous ; y++)
    {
      pdst = &MRIvox(mri_dst, 0, y, z) ;
      plh = &MRIvox(mri_lh, 0, y, z) ;
      prh = &MRIvox(mri_rh, 0, y, z) ;
      for (x = 0 ; x < width && !ambiguous ; x++)
      {
        vlh = *plh++ ;
        vrh = *prh++ ;

        /* if any are ambiguous, mark them all as ambiguous -
           will set seed points later */
        if (vlh >= WM_MIN_VAL && vrh >= WM_MIN_VAL)
        {
          ambiguous = (vlh + vrh)/2 ;
          lh = vlh ;
          rh = vrh ;
          break ;
        }

        v = MAX(vlh, vrh) ;
        *pdst++ = v ;
      }
    }
  }
  if (ambiguous)   /* mark whole brain as ambiguous - will fill seeds later */
  {
    MRI *mri_tmp = NULL ;
    int max_on = 0, min_on = (3*3*3)-1 ;

    for (z = 0 ; z < depth ; z++)
    {
      for (y = 0 ; y < height ; y++)
      {
        pdst = &MRIvox(mri_dst, 0, y, z) ;
        plh = &MRIvox(mri_lh, 0, y, z) ;
        prh = &MRIvox(mri_rh, 0, y, z) ;
        for (x = 0 ; x < width ; x++)
        {
          vlh = *plh++ ;
          vrh = *prh++ ;
          if (vlh >= WM_MIN_VAL || vrh >= WM_MIN_VAL)
            v = ambiguous ;
          else
            v = 0 ;
          *pdst++ = v ;
        }
      }
    }

    fprintf(stderr, 
            "using variable coefficient diffusion "
            "to correct hemispheric overlap...\n") ;

    /* set left hemisphere seed point and nbrs */
    for (zk = -3 ; zk <= 3 ; zk++)
    {
      zi = mri_dst->zi[wm_lh_z+zk] ;
      for (yk = -3 ; yk <= 3 ; yk++)
      {
        yi = mri_dst->yi[wm_lh_y+yk] ;
        for (xk = -3 ; xk <= 3 ; xk++)
        {
          xi = mri_dst->xi[wm_lh_x+xk] ;
          v = MRIvox(mri_dst, xi, yi, zi) ;
          if (v >= WM_MIN_VAL)
            MRIvox(mri_dst, xi, yi, zi) = lh ;
        }
      }
    }
    /* set right hemisphere seed point and nbrs */
    for (zk = -3 ; zk <= 3 ; zk++)
    {
      zi = mri_dst->zi[wm_rh_z+zk] ;
      for (yk = -3 ; yk <= 3 ; yk++)
      {
        yi = mri_dst->yi[wm_rh_y+yk] ;
        for (xk = -3 ; xk <= 3 ; xk++)
        {
          xi = mri_dst->xi[wm_rh_x+xk] ;
          v = MRIvox(mri_dst, xi, yi, zi) ;
          if (v >= WM_MIN_VAL)
            MRIvox(mri_dst, xi, yi, zi) = rh ;
        }
      }
    }
    
    if (Gdiag & DIAG_WRITE && DIAG_VERBOSE_ON)
    {
      fprintf(stderr, "writing ambiguous voxel image to amb.mgh\n") ;
      MRIwrite(mri_dst, "amb.mgh") ;
    }
    mri_tmp = MRIcopy(mri_dst, mri_tmp) ;
    do
    {
      max_on = i = nambiguous = nfilled = 0 ;
      for (z = 0 ; z < depth ; z++)
      {
        for (y = 0 ; y < height ; y++)
        {
          pdst = &MRIvox(mri_dst, 0, y, z) ;
          for (x = 0 ; x < width ; x++)
          {
            if (x == 144 && y == 73 && z == 128)
              DiagBreak() ;
            v = *pdst++ ;
            if (v == ambiguous)
            {
              nleft = nright = 0 ;
              for (zk = -1 ; zk <= 1 ; zk++)
              {
                zi = mri_dst->zi[z+zk] ;
                for (yk = -1 ; yk <= 1 ; yk++)
                {
                  yi = mri_dst->yi[y+yk] ;
                  for (xk = -1 ; xk <= 1 ; xk++)
                  {
                    xi = mri_dst->xi[x+xk] ;
                    v = MRIvox(mri_dst, xi, yi, zi) ;
                    if (v == lh)
                      nleft++ ;
                    else if (v == rh)
                      nright++ ;
                  }
                }
              }
              if (MAX(nright,nleft) > max_on)
                max_on = MIN(min_on, MAX(nright,nleft)) ;
              if (nright >= min_on && nleft >= min_on && nright == nleft)
              {
                if (ODD(i))
                  nleft-- ;
                else
                  nright-- ;
                i++ ;
              }
              if (nright > nleft && nright >= min_on)
              {
                nfilled++ ;
                MRIvox(mri_tmp, x, y, z) = rh ;
                MRIvox(mri_lh, x, y, z) = 0 ;
              }
              else if (nleft > nright && nleft >= min_on)
              {
                nfilled++ ;
                MRIvox(mri_tmp, x, y, z) = lh ;
                MRIvox(mri_rh, x, y, z) = 0 ;
              }
              else
                nambiguous++ ;
            }
          }
        }
      }
      MRIcopy(mri_tmp, mri_dst) ;
      if (min_on != max_on || DIAG_VERBOSE_ON)
        fprintf(stderr, "%d:  %d ambiguous voxels remaining\n", 
                min_on, nambiguous) ;
      min_on = max_on ;
    } while ((nfilled > 0) || (min_on > 0)) ;
  }

  if (Gdiag & DIAG_SHOW && ncorrected > 0)
    fprintf(stderr, "%d overlapping voxels corrected\n", ncorrected) ;
  return(mri_dst) ;
}


static MRI *
mriReadConditionalProbabilities(MRI *mri_T1, char *atlas_name, char *suffix, 
                                int offset, M3D *m3d, MRI *mri_dst)
{
  MRI  *mri_mean, *mri_std, *mri_tmp ;
  char *mri_dir, fname[STRLEN] ;

  mri_dir = getenv("MRI_DIR") ;
  if (!mri_dir)
    ErrorExit(ERROR_BADPARM, "%s: could not read MRI_DIR from environment\n") ;

  if (!mri_dst)
    mri_dst = MRIclone(mri_T1, NULL) ;

  /* read and transform the mean volume */
  sprintf(fname, "%s/average/%s_%s.mgh#%d",
          mri_dir, atlas_name, suffix, offset) ;
  fprintf(stderr, "reading atlas means from %s...\n", fname) ;
  mri_mean = MRIread(fname) ;
  if (!mri_mean)
    ErrorExit(ERROR_NOFILE, "%s: could not read mean file %s",
              Progname, fname) ;
  if (m3d)
  {
    mri_tmp = MRIapplyInverse3DMorph(mri_mean, m3d, NULL) ;
    MRIfree(&mri_mean) ; mri_mean = mri_tmp ;
  }

  /* read and transform the standard deviation volume */
  sprintf(fname, "%s/average/%s_%s.mgh#%d",
          mri_dir, atlas_name, suffix, offset+1) ;
  fprintf(stderr, "reading atlas sigmas from %s...\n", fname) ;
  mri_std = MRIread(fname) ;
  if (!mri_std)
    ErrorExit(ERROR_NOFILE, "%s: could not read std file %s",
              Progname, fname) ;
  if (m3d)
  {
    mri_tmp = MRIapplyInverse3DMorph(mri_std, m3d, NULL) ;
    MRIfree(&mri_std) ; mri_std = mri_tmp ;
  }
  mri_dst = 
    MRIcomputeConditionalProbabilities(mri_T1, mri_mean, mri_std, NULL) ;
  MRIfree(&mri_mean) ; MRIfree(&mri_std) ;
  return(mri_dst) ;
}

static MRI *
mriReadBinaryProbabilities(char *atlas_name, char *suffix, M3D *m3d, 
                           char *subject_name, MRI *mri_dst)
{
  MRI  *mri_priors, *mri_T1, *mri_on_conditional, *mri_off_conditional, *mri_tmp ;
  char *mri_dir, *subjects_dir, fname[STRLEN] ;

  mri_dir = getenv("MRI_DIR") ;
  if (!mri_dir)
    ErrorExit(ERROR_BADPARM, "%s: could not read MRI_DIR from environment\n") ;
  subjects_dir = getenv("SUBJECTS_DIR") ;
  if (!subjects_dir)
    ErrorExit(ERROR_BADPARM, 
              "%s: could not read SUBJECTS_DIR from environment\n") ;

  /* read in subject's T1 volume */
  sprintf(fname, "%s/%s/mri/T1", subjects_dir, subject_name) ;
  fprintf(stderr, "reading T1 volume from %s...\n", fname) ;
  mri_T1 = MRIread(fname) ;
  if (!mri_T1)
    ErrorExit(ERROR_NOFILE, "%s: could not read T1 volume from %s",
              Progname, fname) ;

  /* read and transform the standard deviation volume */
  sprintf(fname, "%s/average/%s_%s.mgh#0", mri_dir, atlas_name, suffix) ;
  fprintf(stderr, "reading atlas priors from %s offset 0...\n", fname) ;
  mri_priors = MRIread(fname) ;
  if (!mri_priors)
    ErrorExit(ERROR_NOFILE, "%s: could not read priors file %s",
              Progname, fname) ;
  if (m3d)
  {
    mri_tmp = MRIapplyInverse3DMorph(mri_priors, m3d, NULL) ;
    MRIfree(&mri_priors) ; mri_priors = mri_tmp ;
  }

  mri_on_conditional = 
    mriReadConditionalProbabilities(mri_T1, atlas_name, suffix, 1, m3d, NULL) ;
  mri_off_conditional = 
    mriReadConditionalProbabilities(mri_T1, atlas_name, suffix, 3, m3d, NULL) ;
  mri_dst = 
    MRIapplyBayesLaw(mri_priors,mri_on_conditional,mri_off_conditional,NULL);
  MRIfree(&mri_T1) ; MRIfree(&mri_on_conditional) ;
  MRIfree(&mri_priors) ; MRIfree(&mri_off_conditional) ;
  return(mri_dst) ;
}

static MRI *
MRIfillVentricle(MRI *mri_src, MRI *mri_prob, MRI *mri_T1, MRI *mri_dst, 
                 float threshold, int out_label, int xabs_min, int xabs_max)
{
  BUFTYPE   *pprob, *pdst, *psrc, out_val, prob, in_val ;
  int       width, height, depth, x, y, z, ymin, ymax, zmin, zmax, 
            nfilled, xk, yk, zk, niter, xmin, xmax, expanded, expansions,
            xi, yi, zi, ventricle_voxels, total_expanded ;

  if (mri_prob->type != MRI_UCHAR)
    ErrorReturn(NULL, (ERROR_UNSUPPORTED, 
                       "MRI3Dthreshold: prob must be MRI_UCHAR")) ;

  if (!mri_dst)
    mri_dst = MRIclone(mri_src, NULL) ;

  width = mri_src->width ; height = mri_src->height ; depth = mri_src->depth ; 
  /* now apply the inverse morph to build an average wm representation
     of the input volume 
     */


  total_expanded = ventricle_voxels = 0 ;
  for (z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      pprob = &MRIvox(mri_prob, 0, y, z) ;
      psrc = &MRIvox(mri_src, 0, y, z) ;
      pdst = &MRIvox(mri_dst, 0, y, z) ;
      for (x = 0 ; x < width ; x++)
      {
        if (DEBUG_POINT(x,y,z))
          DiagBreak() ;
        out_val = 0 ;
        prob = *pprob++ ;   /* value from inverse morphed volume */
        in_val = *psrc++ ;
        if (prob >= threshold)        /* probably on */
          out_val = out_label ;
        else                         /* not sure, use original val */
          out_val = 0 ;

        if (out_val)
          ventricle_voxels++ ;
        *pdst++ = out_val ;
      }
    }
  }

  nfilled = niter = 0 ;
  xmin = width ; ymin = height ; zmin = depth ;
  xmax = ymax = zmax = 0 ;
  do
  {
    expansions = expanded = 0 ;

    /* compute bounding box */
    if (!nfilled)  /* couldn't fill anymore in this bbox - expand it */
    {
      xmin = width ; ymin = height ; zmin = depth ;
      xmax = ymax = zmax = 0 ;
      for (z = 0 ; z < depth ; z++)
      {
        for (y = 0 ; y < height ; y++)
        {
          for (x = 0 ; x < width ; x++)
          {
            if (x == 151 && y == 139 && z == 69)
              DiagBreak() ;
            if (MRIvox(mri_dst, x, y, z) == out_label)
            {
              if (x < xmin)
                xmin = x ;
              if (y < ymin)
                ymin = y ;
              if (z < zmin)
                zmin = z ;
              if (x > xmax)
                xmax = x ;
              if (y > ymax)
                ymax = y ;
              if (z > zmax)
                zmax = z ;
            }
          }
        }
      }
      expanded = 1 ; expansions++ ;
    }
    else
      expanded = 0 ;

    nfilled = 0 ;

    /* don't let bounding box grow outside limits imposed by caller.
       This will (hopefully) prevent the filling from expanding medially
       and escaping into the third ventricle.
    */
    if (xmin < xabs_min)
      xmin = xabs_min ;
    if (xmax > xabs_max)
      xmax = xabs_max ;

    if (!niter++)  /* first time through - shrink filled region */
    {
#define MAX_EXPANSIONS 5
#define BUFFER         10

      for (z = zmin ; z <= zmax ; z++)
      {
        for (y = ymin ; y <= ymax ; y++)
        {
          for (x = xmin ; x <= xmax ; x++)
          {
            /* if close to one of the borders */
            if ((((x-xmin) <= BUFFER) || ((xmax-x) <= BUFFER)) ||
                (((z-zmin) <= BUFFER) || ((zmax-z) <= BUFFER)) ||
                (((y-ymin) <= BUFFER) || ((ymax-y) <= BUFFER)))
            {
              if (x == 151 && y == 139 && z == 69)
                DiagBreak() ;
              if (MRIvox(mri_dst, x, y, z) == out_label)
                MRIvox(mri_dst, x, y, z) = 0 ;
            }
          }
        }
      }
      xmin += BUFFER ; xmax -= BUFFER ;
      ymin += BUFFER ; ymax -= BUFFER ;
      zmin += BUFFER ; zmax -= BUFFER ;
#if 1
      if (out_label == lh_fill_val)
        MRIwrite(mri_dst, "left_ventricle_p_fill.mgh") ;
      else
        MRIwrite(mri_dst, "right_ventricle_p_fill.mgh") ;
#endif
    }

    fprintf(stderr, "ventricle bounding box [%d, %d, %d] --> [%d, %d, %d]\n",
            xmin, ymin, zmin, xmax, ymax, zmax) ;
    for (z = zmin ; z <= zmax ; z++)
    {
      for (y = ymin ; y <= ymax ; y++)
      {
        for (x = xmin ; x <= xmax ; x++)
        {
          if (x == 122 && y == 109 && z == 117)
            DiagBreak() ;
          if (MRIvox(mri_dst, x, y, z) == out_label)
          {   /* a filled voxel - expand fill */
            for (zk = -1 ; zk <= 1 ; zk++)
            {
              zi = mri_dst->zi[z+zk] ;
              for (yk = -1 ; yk <= 1 ; yk++)
              {
                yi = mri_dst->yi[y+yk] ;
                for (xk = -1 ; xk <= 1 ; xk++)
                {
                  xi = mri_dst->xi[x+xk] ;
                  if (x == 122 && y == 108 && z == 117)
                    DiagBreak() ;
                  if ((MRIvox(mri_T1, xi, yi, zi) < 50) &&
                      (MRIvox(mri_dst, xi, yi, zi) != out_label))
                  {
                    MRIvox(mri_dst, xi, yi, zi) = out_label ;
                    nfilled++ ;
                  }
                }
              }
            }
          }
        }
      }
    }
    fprintf(stderr, "%d ventricular voxels filled\n", nfilled) ;
    total_expanded += nfilled ;

    if ((ventricle_voxels + total_expanded > 20000) ||
        (++expansions >= MAX_EXPANSIONS))
      break ;

  } while (!expanded || (nfilled > 0)) ;
  
  ventricle_voxels += total_expanded ;
  fprintf(stderr, "%5d ventricular voxels, %5d from atlas %5d from fill.\n", 
          ventricle_voxels, ventricle_voxels-total_expanded, total_expanded);

  return(mri_dst) ;
}

static int is_diagonal(MRI *mri, int x0, int y0, int z0, int block[2][2][2]) ;
static int count_diagonals(MRI *mri, int x0, int y0, int z0) ;
static int count_voxel_diagonals(MRI *mri, int x0, int y0, int z0) ;
static int fillDiagonals(MRI *mri, int fillval) ;

static int
MRIfillDegenerateLocations(MRI *mri, int fillval)
{
  int nfilled, total_filled ;

  total_filled = 0 ;
  do
  {
    nfilled = fillDiagonals(mri, fillval) ;
    total_filled += nfilled ;
    fprintf(stderr, "%d voxels filled\n", nfilled) ;
  } while(nfilled > 0) ;

  return(NO_ERROR) ;
}
static int
fillDiagonals(MRI *mri, int fillval)
{
  int  nfilled, x, y, z, width, height, depth, block[2][2][2], xk, yk, zk,
       x1, y1, z1, mxk, myk, mzk, diagonals, min_diagonals, filled ;

  nfilled = 0 ;

  width = mri->width ; height = mri->height ; depth = mri->depth ;
  for (z = 1 ; z < (depth-1) ; z++)
  {
    for (y = 1 ; y < (height-1) ; y++)
    {
      for (x = 1 ; x < (width-1) ; x++)
      {
        while (is_diagonal(mri, x, y, z, block))
        {
          /* find one voxel to fill */
          min_diagonals = 10000 ;
          mxk = myk = mzk = 0 ;
          for (zk = 0 ; zk <= 1 ; zk++)
          {
            z1 = z + zk ;
            for (yk = 0 ; yk <= 1 ; yk++)
            {
              y1 = y + yk ;
              for (xk = 0 ; xk <= 1 ; xk++)
              {

                /* should arbitrate here - try to find non-diagonal fill */
                if (block[xk][yk][zk])
                {
                  x1 = x + xk ;
                  if (MRIvox(mri, x1, y1, z1) != fillval)
                  {
                    MRIvox(mri, x1, y1, z1) = fillval ;
                    filled = 1 ;
                  }
                  else
                    filled = 0 ;
                  diagonals = count_voxel_diagonals(mri, x1, y1, z1) ;
                  if (diagonals <= min_diagonals)
                  {
                    min_diagonals = diagonals ;
                    mxk = xk ; myk = yk ; mzk = zk ;
                  }
                  if (filled)
                    MRIvox(mri, x1, y1, z1) = 0 ;
                }
              }
            }
          }
          MRIvox(mri, x+mxk, y+myk, z+mzk) = fillval ;
          nfilled++ ;
        }
      }
    }
  }
  return(nfilled) ;
}

static int
is_diagonal(MRI *mri, int x0, int y0, int z0, int block[2][2][2])
{
  int   non, x, y, z, d, dsign, diagonal = 0 ;

  /* do quick check to see if it is possible */
  for (non = 0, z = z0 ; z <= z0+1 ; z++)
    for (y = y0 ; y <= y0+1 ; y++)
      for (x = x0 ; x <= x0+1 ; x++)
        if (MRIvox(mri,x,y,z))
          non++ ;

  if (non < 2 || non > 6)
    return(0) ;
  
  /* now compute x derivatives for potential filling locations */
  for (dsign = 0, z = z0 ; z <= z0+1 ; z++)
  {
    for (y = y0 ; y <= y0+1 ; y++)
    {
      for (x = x0 ; x <= x0+1 ; x++)
      {
        if (MRIvox(mri,x,y,z) < WM_MIN_VAL)
        {
          d = MRIvox(mri, x0+1,y,z) - MRIvox(mri, x0,y,z) ;
          block[x-x0][y-y0][z-z0] = d ;
          if (d*dsign < 0)
            diagonal = 1 ;  /* opposite sign spatial derivatives - diagonal */
          else
            dsign = d ;
        }
        else
          block[x-x0][y-y0][z-z0] = 0 ;
      }
    }
  }

  if (diagonal)
    return(1) ;

  /* now compute y derivatives for potential filling locations */
  for (dsign = 0, z = z0 ; z <= z0+1 ; z++)
  {
    for (y = y0 ; y <= y0+1 ; y++)
    {
      for (x = x0 ; x <= x0+1 ; x++)
      {
        if (MRIvox(mri,x,y,z) < WM_MIN_VAL)
        {
          d = MRIvox(mri, x,y0+1,z) - MRIvox(mri, x,y0,z) ;
          block[x-x0][y-y0][z-z0] = d ;
          if (d*dsign < 0)
            diagonal = 1 ;  /* opposite sign spatial derivatives - diagonal */
          else
            dsign = d ;
        }
        else
          block[x-x0][y-y0][z-z0] = 0 ;
      }
    }
  }

  if (diagonal)
    return(1) ;

  /* now compute y derivatives for potential filling locations */
  for (dsign = 0, z = z0 ; z <= z0+1 ; z++)
  {
    for (y = y0 ; y <= y0+1 ; y++)
    {
      for (x = x0 ; x <= x0+1 ; x++)
      {
        if (MRIvox(mri,x,y,z) < WM_MIN_VAL)
        {
          d = MRIvox(mri, x,y,z0+1) - MRIvox(mri, x,y,z0+1) ;
          block[x-x0][y-y0][z-z0] = d ;
          if (d*dsign < 0)
            diagonal = 1 ;  /* opposite sign spatial derivatives - diagonal */
          else
            dsign = d ;
        }
        else
          block[x-x0][y-y0][z-z0] = 0 ;
      }
    }
  }

  return(diagonal) ;
}
static int
count_voxel_diagonals(MRI *mri, int x0, int y0, int z0)
{
  int diagonals = 0, xk, yk, zk ;

  for (zk = -1 ; zk <= 0 ; zk++)
  for (yk = -1 ; yk <= 0 ; yk++)
  for (xk = -1 ; xk <= 0 ; xk++)
    diagonals += count_diagonals(mri, x0+xk, y0+yk, z0+zk) ;
  return(diagonals) ;
}
static int
count_diagonals(MRI *mri, int x0, int y0, int z0) 
{
  int   non, x, y, z, d, dsign, diagonals = 0 ;

  /* do quick check to see if it is possible */
  for (non = 0, z = z0 ; z <= z0+1 ; z++)
    for (y = y0 ; y <= y0+1 ; y++)
      for (x = x0 ; x <= x0+1 ; x++)
        if (MRIvox(mri,x,y,z))
          non++ ;

  if (non < 2 || non > 6)
    return(0) ;
  
  /* now compute x derivatives for potential filling locations */
  for (dsign = 0, z = z0 ; z <= z0+1 ; z++)
  {
    for (y = y0 ; y <= y0+1 ; y++)
    {
      for (x = x0 ; x <= x0+1 ; x++)
      {
        if (MRIvox(mri,x,y,z) < WM_MIN_VAL)
        {
          d = MRIvox(mri, x0+1,y,z) - MRIvox(mri, x0,y,z) ;
          if (d*dsign < 0)
            diagonals++ ;  /* opposite sign spatial derivatives - diagonal */
          else
            dsign = d ;
        }
      }
    }
  }

  /* now compute y derivatives for potential filling locations */
  for (dsign = 0, z = z0 ; z <= z0+1 ; z++)
  {
    for (y = y0 ; y <= y0+1 ; y++)
    {
      for (x = x0 ; x <= x0+1 ; x++)
      {
        if (MRIvox(mri,x,y,z) < WM_MIN_VAL)
        {
          d = MRIvox(mri, x,y0+1,z) - MRIvox(mri, x,y0,z) ;
          if (d*dsign < 0)
            diagonals++ ;  /* opposite sign spatial derivatives - diagonal */
          else
            dsign = d ;
        }
      }
    }
  }

  /* now compute y derivatives for potential filling locations */
  for (dsign = 0, z = z0 ; z <= z0+1 ; z++)
  {
    for (y = y0 ; y <= y0+1 ; y++)
    {
      for (x = x0 ; x <= x0+1 ; x++)
      {
        if (MRIvox(mri,x,y,z) < WM_MIN_VAL)
        {
          d = MRIvox(mri, x,y,z0+1) - MRIvox(mri, x,y,z0+1) ;
          if (d*dsign < 0)
            diagonals++ ;  /* opposite sign spatial derivatives - diagonal */
          else
            dsign = d ;
        }
      }
    }
  }

  return(diagonals) ;
}
static int
edit_segmentation(MRI *mri_wm, MRI *mri_seg)
{
  int   width, height, depth, x, y, z, label, non, noff, xi, yi, zi,  xk, yk, zk, nchanged, wsize ;
  MRI   *mri_filled ;

  mri_filled =  MRIclone(mri_wm,  NULL);

  width = mri_wm->width ; height = mri_wm->height ; depth = mri_wm->depth ;

  non = noff = 0 ;
  for (z = 0 ; z < depth ; z++)
  {
    for (y = 0 ; y < height ; y++)
    {
      for (x = 0 ; x < width ; x++)
      {
        if (x == Gx && y == Gy && z == Gz)  
           DiagBreak() ;
        label = MRIvox(mri_seg, x, y, z) ;
        
        switch (label)
        {
	case Unknown:
	  wsize=5 ;
	  if (MRIlabelsInNbhd(mri_seg, x, y, z, (wsize-1)/2, Unknown) < (wsize*wsize*wsize-1))
	    break ;
	  
	  /* !!! no break - erase unknown if it is surrounded by only  unknowns */
	  
	  /* erase these  labels */
        case Left_Cerebellum_White_Matter:
        case Left_Cerebellum_Exterior:
        case Left_Cerebellum_Cortex:
        case Right_Cerebellum_White_Matter:
        case Right_Cerebellum_Exterior:
        case Right_Cerebellum_Cortex:
        case Right_Cerebral_Cortex:
        case Brain_Stem:
        case Left_VentralDC:
        case Right_VentralDC:
        case Left_Substancia_Nigra:
        case Right_Substancia_Nigra:
          if ((neighborLabel(mri_seg, x,y,z,1,Left_Cerebral_Cortex) == 0) &&
              (neighborLabel(mri_seg, x,y,z,1,Right_Cerebral_Cortex) == 0))
          {
            if (MRIvox(mri_im, x, y, z))
            {
              MRIvox(mri_wm, x, y, z) = 0 ;
              noff++ ;
            }
          }
          break ;
	  
	  /* fill these */
	case Left_Lesion:
	case Right_Lesion:
	case WM_hypointensities:
	case Left_WM_hypointensities:
	case Right_WM_hypointensities:
	case non_WM_hypointensities:
	case Left_non_WM_hypointensities:
	case Right_non_WM_hypointensities:
          if ((neighborLabel(mri_seg, x, y, z,1,Left_Cerebral_Cortex) >= 0) &&
              (neighborLabel(mri_seg, x, y, z,1,Right_Cerebral_Cortex) >= 0) &&
              (MRIvox(mri_wm, x, y, z) < WM_MIN_VAL))
          {
            MRIvox(mri_wm, x, y, z) = 255 ;
            MRIvox(mri_filled, x, y, z) = 255 ;
            non++ ;  
          }
					break ;
        case Left_Lateral_Ventricle:
        case Right_Lateral_Ventricle:
        case Left_Inf_Lat_Vent:
        case Right_Inf_Lat_Vent:
          if ((neighborLabel(mri_seg, x, y, z,1,Left_Cerebral_Cortex) >= 0) &&
              (neighborLabel(mri_seg, x, y, z,1,Right_Cerebral_Cortex) >= 0) &&
              (MRIvox(mri_wm, x, y, z) < WM_MIN_VAL))
          {
            MRIvox(mri_wm, x, y, z) = 255 ;
            MRIvox(mri_filled, x, y, z) = 255 ;
            non++ ;  
          }
          yi = mri_wm->yi[y+1] ;
          label = MRIvox(mri_seg, x,yi, z) ;
          if (((label == Left_Cerebral_Cortex || label == Right_Cerebral_Cortex) ||
							 (label == Left_Cerebral_White_Matter || label == Right_Cerebral_White_Matter) ||
							 (label == Unknown))
              && (MRIvox(mri_wm, x, yi, z) < WM_MIN_VAL))
          {
            MRIvox(mri_wm, x, yi, z) = 255 ;
            MRIvox(mri_filled, x, yi, z) = 255 ;
            non++ ;
          }
          break ;
        case Left_Hippocampus:
        case Right_Hippocampus:
	  {
	    int xi,  olabel ;
	    
	    xi = label == Right_Hippocampus ?  mri_wm->xi[x-1] :  mri_wm->xi[x+1] ;
	    olabel = MRIvox(mri_seg, xi, y, z) ;
	    if (olabel != label)  /* voxel lateral to this one is not hippocampus   */
	    {
	      MRIvox(mri_wm, xi, y, z) = 255 ;
	      MRIvox(mri_filled, xi, y, z) = 255 ;
	      non++ ;
	    }
	    
	    yi = mri_wm->yi[y+1] ;
	    label = MRIvox(mri_seg, x,yi, z) ;
	    if (((label == Left_Cerebral_Cortex || label == Right_Cerebral_Cortex) ||
		 (label == Left_Cerebral_White_Matter || label == Right_Cerebral_White_Matter))
		&& (MRIvox(mri_wm, x, yi, z) < WM_MIN_VAL))
	    {
	      MRIvox(mri_wm, x, yi, z) = 255 ;
	      MRIvox(mri_filled, x, yi, z) = 255 ;
	      yi = mri_wm->yi[y+2] ;
	      MRIvox(mri_wm, x, yi, z) = 255 ;
	      MRIvox(mri_filled, x, yi, z) = 255 ;
	      non += 2 ;
	    }
	    break ;
				}
        case Left_Accumbens_area:
        case Right_Accumbens_area:
        case Left_Caudate:
        case Right_Caudate:
        case Left_Putamen:
        case Right_Putamen:
        case Left_Pallidum:
        case Right_Pallidum:
          if (MRIvox(mri_wm, x, y, z) < WM_MIN_VAL)
          {
            MRIvox(mri_wm, x, y, z) = 255 ;
            MRIvox(mri_filled, x, y, z) = 255 ;
            non++ ; 
          }
          break ;
        default:
          break ;
        }
      }
    }
  }

  /*
    fill in voxels that were labeled wm by the aseg, but not by  wmfilter, and are
    neighbors  of  voxels that  have been already been filled .
  */
  do
  {
    nchanged = 0 ;
    for (z = 0 ; z < depth ; z++)
    {
      for (y = 0 ; y < height ; y++)
      {
	for (x = 0 ; x < width ; x++)
	{
	  if (x == Gx && y == Gy && z == Gz)  
	    DiagBreak() ;
	  if  (MRIvox(mri_filled, x,  y, z) == 0)
	    continue  ;
	  for (xk = -1 ; xk <= 1 ; xk++)
	  {
	    xi = mri_filled->xi[x+xk] ;
	    for (yk = -1 ; yk <= 1 ; yk++)
	    {
	      yi = mri_filled->yi[y+yk] ;
	      for (zk = -1 ; zk <= 1 ; zk++)
	      {
		zi = mri_filled->zi[z+zk] ;
		if (xi == Gx && yi == Gy && zi == Gz)  
		  DiagBreak() ;
		label = MRIvox(mri_seg, xi, yi, zi) ;
		if (IS_WM(label) &&  (MRIvox(mri_wm, xi, yi, zi) < WM_MIN_VAL))
		{
		  nchanged++ ;
		  MRIvox(mri_wm, xi, yi, zi) = 255 ;
#if 0
		  MRIvox(mri_filled, xi, yi, zi) = 255 ;
#endif
		  non++ ;  
		}
	      }
	    }
	  }
	}
      }
    }
    printf("%d additional wm voxels added\n", nchanged)  ;
  } while (nchanged >  0) ;
  
  printf("SEG EDIT: %d voxels turned on, %d voxels turned off.\n", non, noff) ;
	MRIfree(&mri_filled) ;
  return(NO_ERROR) ;
}

#if 0
static int
neighbors(MRI *mri, int x, int y,int z,int whalf,int label)
{
  int xi, yi, zi, xk, yk, zk, nbrs ;
  
  for (nbrs = 0, zk = -whalf ; zk <= whalf ; zk++)
  {
    zi = mri->zi[z+zk] ;
    for (yk = -whalf ; yk <= whalf ; yk++)
    {
      yi = mri->yi[y+yk] ;
      for (xk = -whalf ; xk <= whalf ; xk++)
      {
        if (abs(xk)+abs(yk)+abs(zk) > 1) /* only 6-connected neighbors */
          continue ;
        xi = mri->xi[x+xk] ;
        if (MRIvox(mri, xi, yi, zi) == label)
          nbrs++ ;
      }
    }
  }
  return(nbrs) ;
}
#endif

static int
neighborLabel(MRI *mri, int x, int y, int z, int whalf, int label)
{
  int xi, yi, zi, xk, yk, zk ;
  
  for (zk = -whalf ; zk <= whalf ; zk++)
  {
    zi = mri->zi[z+zk] ;
    for (yk = -whalf ; yk <= whalf ; yk++)
    {
      yi = mri->yi[y+yk] ;
      for (xk = -whalf ; xk <= whalf ; xk++)
      {
        if (abs(xk)+abs(yk)+abs(zk) > 1) /* only 6-connected neighbors */
          continue ;
        xi = mri->xi[x+xk] ;
        if (MRIvox(mri, xi, yi, zi) == label)
          return(1) ;
      }
    }
  }
  return(0) ;
}

static int
MRIlabelsInNbhd(MRI *mri, int x, int y, int z, int whalf, int label)
{
  int xi, yi, zi, xk, yk, zk, count ;
  
  for (count = 0, zk = -whalf ; zk <= whalf ; zk++)
  {
    zi = mri->zi[z+zk] ;
    for (yk = -whalf ; yk <= whalf ; yk++)
    {
      yi = mri->yi[y+yk] ;
      for (xk = -whalf ; xk <= whalf ; xk++)
      {
        xi = mri->xi[x+xk] ;
        if (MRIvox(mri, xi, yi, zi) == label)
	  count++;
      }
    }
  }
  return(count) ;
}

static MRI *
extend_to_lateral_borders(MRI *mri_src, MRI *mri_dst, int mask)
{
  int   x, y, z, found ;

  if (!mri_dst)
    mri_dst = MRIclone(mri_src, NULL) ;

  for (y = 0 ; y < mri_dst->height ; y++)
  {
    for (z = 0 ; z < mri_dst->depth ; z++)
    {
      // look for mask value
      for (found = x = 0 ; x < mri_dst->width ; x++)
      {
        if (MRIvox(mri_dst, x, y, z) == mask)  /* fill in whole line */
        {
          found = 1 ;
          break ;
        }
      }
      if (found)
      {
        for (x = 0 ; x < mri_dst->width ; x++)
        {
          MRIvox(mri_dst, x, y, z) = mask ;
        }
      }
    }
  }

  return(mri_dst) ;
}

static int
find_cc_seed_with_segmentation(MRI *mri, MRI *mri_seg, Real *pcc_tal_x, Real *pcc_tal_y, Real *pcc_tal_z)
{
  int  x,  y, z, label, npoints, olabel ;
  Real xcc, ycc,  zcc ;
  float  dist,min_dist, min_x, min_y,min_z;
  
  xcc = ycc = zcc = 0.0  ; npoints = 0 ;
  for (x = 0 ; x  < mri->width ; x++)
  {
    for (y = 0 ; y  < mri->height ; y++)
    {
      for (z = 0 ; z  < mri->width ; z++)
      {
	label = MRIvox(mri_seg, x,  y, z) ;
	if (!IS_WM(label))
	  continue  ;
	if (label == Left_Cerebral_White_Matter)
	  olabel =  Right_Cerebral_White_Matter ;
	else
	  olabel =  Left_Cerebral_White_Matter ;
	
	if (neighborLabel(mri_seg, x, y, z,  1, olabel) > 0)
	{
	  xcc +=  x ; ycc += y ; zcc += z ; npoints++ ;
	}
      }
    }
  }
  if (npoints <= 0)
    ErrorExit(ERROR_BADFILE, "%s: could not find any points where lh and rh wm  are nbrs", Progname) ;
  
  xcc /= (Real)npoints  ; ycc /= (Real)npoints ; zcc /= (Real)npoints ;
  
  /* now search for the point closest to the centroid that is a  wm point and satisfies the lh/rh constraint */
  min_dist  = 1000000 ; min_x = xcc ; min_y = ycc ;  min_z = zcc  ;
  for (x = 0 ; x  < mri->width ; x++)
  {
    for (y = 0 ; y  < mri->height ; y++)
    {
      for (z = 0 ; z  < mri->width ; z++)
      {
	label = MRIvox(mri_seg, x,  y, z) ;
	if (!IS_WM(label) || (MRIvox(mri, x, y, z)  < WM_MIN_VAL))
	  continue  ;
	if (label == Left_Cerebral_White_Matter)
	  olabel =  Right_Cerebral_White_Matter ;
	else
	  olabel =  Left_Cerebral_White_Matter ;
	
	if (neighborLabel(mri_seg, x, y, z,  1, olabel) > 0)
	{
	  dist = SQR(x-xcc) + SQR(y-ycc) + SQR(z-zcc)  ;
	  if (dist <  min_dist)
	  {
	    min_dist  = dist ;
	    min_x = x ; min_y = y ; min_z = z ;
	  }
	}
      }
    }
  }
  
  MRIvoxelToWorld(mri, min_x, min_y, min_z, pcc_tal_x, pcc_tal_y, pcc_tal_z) ;
  printf("segmentation indicates cc at (%d,  %d,  %d) --> (%2.1f, %2.1f, %2.1f)\n",
	 nint(xcc), nint(ycc), nint(zcc), *pcc_tal_x, *pcc_tal_y, *pcc_tal_z) ;
  
  return(NO_ERROR) ;
}

