/*!
 * \file
 * \brief
 * \author Micha Laszkowski
 */

#include <memory>
#include <string>

#include "LUMGenerator.hpp"
#include "Common/Logger.hpp"

#include <boost/bind.hpp>
///////////////////////////////////////////
#include <string>
#include <cmath>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <pcl/point_representation.h>

#include "pcl/impl/instantiate.hpp"
#include "pcl/search/kdtree.h"
#include "pcl/search/impl/kdtree.hpp"
#include <pcl/registration/correspondence_estimation.h>
#include "pcl/registration/correspondence_rejection_sample_consensus.h"
#include <pcl/registration/lum.h>

#include <pcl/io/pcd_io.h>
#include <pcl/visualization/cloud_viewer.h>
#include <pcl/visualization/point_cloud_color_handlers.h>
////////////////////////////////////////////////////////////////////////
#include <pcl/filters/filter.h>

#include<pcl/registration/icp.h>
#include <pcl/registration/icp_nl.h>

#include <pcl/filters/voxel_grid.h>
#include <pcl/features/normal_3d.h>




namespace Processors {
namespace LUMGenerator {



//convenient typedefs
typedef pcl::PointXYZRGB PointT;
typedef pcl::PointCloud<PointT> PointCloud;
typedef pcl::PointNormal PointNormalT;
typedef pcl::PointCloud<PointNormalT> PointCloudWithNormals;

// Define a new point representation for < x, y, z, curvature >
class MyPointRepresentation : public pcl::PointRepresentation <PointNormalT>
{
  using pcl::PointRepresentation<PointNormalT>::nr_dimensions_;
public:
  MyPointRepresentation ()
  {
    // Define the number of dimensions
    nr_dimensions_ = 4;
  }

  // Override the copyToFloatArray method to define our feature vector
  virtual void copyToFloatArray (const PointNormalT &p, float * out) const
  {
    // < x, y, z, curvature >
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
    out[3] = p.curvature;
  }
};

/*
 * \brief Class used for transformation from SIFT descriptor to array of floats.
 */
class SIFTFeatureRepresentation: public pcl::DefaultFeatureRepresentation <PointXYZSIFT> //could possibly be pcl::PointRepresentation<...> ??
{
	/// Templatiated number of SIFT descriptor dimensions.
	using pcl::PointRepresentation<PointXYZSIFT>::nr_dimensions_;

	public:
	SIFTFeatureRepresentation ()
	{
		// Define the number of dimensions.
		nr_dimensions_ = 128 ;
		trivial_ = false ;
	}

	/// Overrides the copyToFloatArray method to define our feature vector
	virtual void copyToFloatArray (const PointXYZSIFT &p, float * out) const
	{
		//This representation is only for determining correspondences (not for use in Kd-tree for example - so use only the SIFT part of the point.
		for (register int i = 0; i < 128 ; i++)
			out[i] = p.descriptor[i];//p.descriptor.at<float>(0, i) ;
	}
};


Eigen::Matrix4f LUMGenerator::computeTransformationSAC(const pcl::PointCloud<PointXYZSIFT>::ConstPtr &cloud_src, const pcl::PointCloud<PointXYZSIFT>::ConstPtr &cloud_trg,
		const pcl::CorrespondencesConstPtr& correspondences, pcl::Correspondences& inliers)
{
//	CLOG(LTRACE) << "Computing SAC" << std::endl ;

	pcl::registration::CorrespondenceRejectorSampleConsensus<PointXYZSIFT> sac ;
	sac.setInputSource(cloud_src) ;
	sac.setInputTarget(cloud_trg) ;
	sac.setInlierThreshold(0.01f) ; //property RanSAC
	sac.setMaximumIterations(2000) ; //property RanSAC
	sac.setInputCorrespondences(correspondences) ;
	sac.getCorrespondences(inliers) ;

	CLOG(LINFO) << "SAC inliers " << inliers.size();

	return sac.getBestTransformation() ;
}


LUMGenerator::LUMGenerator(const std::string & name) :
    Base::Component(name),
    prop_ICP_alignment("ICP.Iterative", false),
	threshold("threshold", 5),
	maxIterations("Interations.Max", 5)
{
	registerProperty(maxIterations);
	registerProperty(threshold);
    registerProperty(prop_ICP_alignment);

}

LUMGenerator::~LUMGenerator() {
}


void LUMGenerator::prepareInterface() {
	// Register data streams.
	registerStream("in_cloud_xyzrgb", &in_cloud_xyzrgb);
	registerStream("in_cloud_xyzsift", &in_cloud_xyzsift);
	registerStream("out_instance", &out_instance);
	registerStream("out_cloud_xyzrgb", &out_cloud_xyzrgb);
	registerStream("out_cloud_xyzsift", &out_cloud_xyzsift);
	registerStream("out_mean_viewpoint_features_number", &out_mean_viewpoint_features_number);
	//registerStream("out_trigger", &out_Trigger);
	//registerStream("in_trigger", &in_trigger);

    // Register single handler - the "addViewToModel" function.
    h_addViewToModel.setup(boost::bind(&LUMGenerator::addViewToModel, this));
    registerHandler("addViewToModel", &h_addViewToModel);
    addDependency("addViewToModel", &in_cloud_xyzsift);
    addDependency("addViewToModel", &in_cloud_xyzrgb);



   // h_Trigger.setup(Trigger::trigger(),this);
   // registerHandler("Trigger", &h_Trigger);
	//addDependency("Trigger", &h_Trigger);

	//h_Trigger.setup(boost::bind(&LUMGenerator::out_trigger, this));
	//registerHandler("trigger", &h_Trigger);
	//addDependency("trigger", &out_Trigger);
	//addDependency("trigger", &in_trigger);

}

bool LUMGenerator::onInit() {
	// Number of viewpoints.
	counter = 0;
	// Mean number of features per view.
	mean_viewpoint_features_number = 0;

	global_trans = Eigen::Matrix4f::Identity();

	cloud_merged = pcl::PointCloud<pcl::PointXYZRGB>::Ptr (new pcl::PointCloud<pcl::PointXYZRGB>());
	cloud_sift_merged = pcl::PointCloud<PointXYZSIFT>::Ptr (new pcl::PointCloud<PointXYZSIFT>());
/*
	cloud_prev = pcl::PointCloud<pcl::PointXYZRGB>::Ptr (new pcl::PointCloud<pcl::PointXYZRGB>());
	cloud_next = pcl::PointCloud<pcl::PointXYZRGB>::Ptr (new pcl::PointCloud<pcl::PointXYZRGB>());
	cloud_sift_prev = pcl::PointCloud<PointXYZSIFT>::Ptr (new pcl::PointCloud<PointXYZSIFT>());
	cloud_sift_next = pcl::PointCloud<PointXYZSIFT>::Ptr (new pcl::PointCloud<PointXYZSIFT>());
*/

	return true;
}

bool LUMGenerator::onFinish() {
	return true;
}

bool LUMGenerator::onStop() {
	return true;
}

bool LUMGenerator::onStart() {
	return true;
}

bool loopDetection (int end, std::vector<pcl::PointCloud<pcl::PointXYZRGB>::Ptr> &clouds, double dist, int &first, int &last)
{
  static double min_dist = -1;
  int state = 0;

  for (int i = end-1; i >= 0; i--)
  {
    Eigen::Vector4f cstart, cend;
    pcl::compute3DCentroid (*(clouds[i]), cstart);
    pcl::compute3DCentroid (*(clouds[end]), cend);
    Eigen::Vector4f diff = cend - cstart;
    double norm = diff.norm ();

    std::cout << endl << "distance between " << i << " and " << end << " is " << norm << " state is " << state << endl;

    if (state == 0 && norm > dist)
    {
      state = 1;
      //std::cout << "state 1" << std::endl;
    }
    if (state > 0 && norm < dist)
    {
      state = 2;
      std::cout << "loop detected between scan " << i << " and scan " << end  << endl;
      if (min_dist < 0 || norm < min_dist)
      {
        min_dist = norm;
        first = i;
        last = end;
      }
    }
  }
  //std::cout << "min_dist: " << min_dist << " state: " << state << " first: " << first << " end: " << end << std::endl;
   if (min_dist > 0 && (state < 2 || end == int (clouds.size ()) - 1)) //TODO
   {
     min_dist = -1;
     return true;
   }
   return false;
 }

void LUMGenerator::addViewToModel() {
    CLOG(LTRACE) << "LUMGenerator::addViewToModel";

	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud = in_cloud_xyzrgb.read();
	pcl::PointCloud<PointXYZSIFT>::Ptr cloud_sift = in_cloud_xyzsift.read();

	// TODO if empty()

	CLOG(LINFO) << "cloud_xyzrgb size: "<<cloud->size();
	CLOG(LINFO) << "cloud_xyzsift size: "<<cloud_sift->size();

	// Remove NaNs.
	std::vector<int> indices;
	cloud->is_dense = false;
	pcl::removeNaNFromPointCloud(*cloud, *cloud, indices);
	cloud_sift->is_dense = false;
	pcl::removeNaNFromPointCloud(*cloud_sift, *cloud_sift, indices);

	CLOG(LDEBUG) << "cloud_xyzrgb size without NaN: "<<cloud->size();
	CLOG(LDEBUG) << "cloud_xyzsift size without NaN: "<<cloud_sift->size();

	CLOG(LINFO) << "view number: "<<counter;
	CLOG(LINFO) << "view cloud->size(): "<<cloud->size();
	CLOG(LINFO) << "view cloud_sift->size(): "<<cloud_sift->size();

	rgb_views.push_back(pcl::PointCloud<pcl::PointXYZRGB>::Ptr (new pcl::PointCloud<pcl::PointXYZRGB>()));

	counter++;

		if (counter > threshold) {
			lum_sift.setMaxIterations(maxIterations);
			lum_sift.compute();
			CLOG(LINFO) << "ended";
			CLOG(LINFO) << "cloud_merged from LUM ";
			*cloud_merged = *(rgb_views[0]);
			for (int i = 1 ; i < threshold; i++)
			{
				pcl::PointCloud<pcl::PointXYZRGB> tmp = *(rgb_views[i]);
				pcl::transformPointCloud(tmp, tmp, lum_sift.getTransformation (i));
				*cloud_merged += tmp;
			}

			CLOG(LINFO) << "model cloud->size(): "<< cloud_merged->size();
			CLOG(LINFO) << "model cloud_sift->size(): "<< cloud_sift_merged->size();


			// Compute mean number of features.
			mean_viewpoint_features_number = total_viewpoint_features_number/threshold;

			out_cloud_xyzrgb.write(cloud_merged);
			out_cloud_xyzsift.write(cloud_sift_merged);
			cloud_sift_merged = lum_sift.getConcatenatedCloud ();
			return;
		}

	// First cloud.
	if (counter == 1)
	{
		//counter++;
		mean_viewpoint_features_number = cloud_sift->size();
		// Push results to output data ports.
		out_mean_viewpoint_features_number.write(mean_viewpoint_features_number);

		lum_sift.addPointCloud(cloud_sift);
		*rgb_views[0] = *cloud;


		*cloud_merged = *cloud;
		*cloud_sift_merged = *cloud_sift;

		out_cloud_xyzrgb.write(cloud_merged);
		out_cloud_xyzsift.write(cloud_sift_merged);
		// Push SOM - depricated.
//		out_instance.write(produce());
		CLOG(LINFO) << "return ";
		return;
	}

//	 Find corespondences between feature clouds.
//	 Initialize parameters.
	pcl::CorrespondencesPtr correspondences(new pcl::Correspondences()) ;
	pcl::registration::CorrespondenceEstimation<PointXYZSIFT, PointXYZSIFT> correst;
	SIFTFeatureRepresentation::Ptr point_representation2(new SIFTFeatureRepresentation()) ;
	correst.setPointRepresentation(point_representation2) ;
	correst.setInputSource(cloud_sift) ;
	correst.setInputTarget(cloud_sift_merged) ;
	// Find correspondences.
	correst.determineReciprocalCorrespondences(*correspondences) ;
	CLOG(LINFO) << "Number of reciprocal correspondences: " << correspondences->size() << " out of " << cloud_sift->size() << " features";


    // Compute transformation between clouds and SOMGenerator global transformation of cloud.
	pcl::Correspondences inliers;
	Eigen::Matrix4f current_trans = computeTransformationSAC(cloud_sift, cloud_sift_merged, correspondences, inliers);

	pcl::transformPointCloud(*cloud, *cloud, current_trans);
	pcl::transformPointCloud(*cloud_sift, *cloud_sift, current_trans);

	lum_sift.addPointCloud(cloud_sift);
	*rgb_views[counter -1] = *cloud;
//	*cloud_sift_merged += *cloud_sift;

//	loopDetection(counter-1, rgb_views, 0.03, first, last);
	int added = 0;
	for (int i = counter - 2 ; i >= 0; i--)
	{
		pcl::CorrespondencesPtr correspondences2(new pcl::Correspondences()) ;
		pcl::registration::CorrespondenceEstimation<PointXYZSIFT, PointXYZSIFT> correst2;
		SIFTFeatureRepresentation::Ptr point_representation(new SIFTFeatureRepresentation()) ;
		correst2.setPointRepresentation(point_representation) ;
		correst2.setInputSource(lum_sift.getPointCloud(counter - 1));
		correst2.setInputTarget(lum_sift.getPointCloud(i));
		// Find correspondences.
		correst2.determineReciprocalCorrespondences(*correspondences2);
		//CLOG(LINFO) << "Number of reciprocal correspondences: " << correspondences2->size() << " out of " << lum_sift.getPointCloud(counter - 1)->size() << " features";

		pcl::CorrespondencesPtr correspondences3(new pcl::Correspondences()) ;
		computeTransformationSAC(lum_sift.getPointCloud(counter - 1), lum_sift.getPointCloud(i), correspondences2, *correspondences3) ;
		//cortab[counter-1][i] = inliers2;
		if (correspondences3->size() > 10) {
			lum_sift.setCorrespondences(counter-1, i, correspondences3);
			added++;
		}
	//break;
	//CLOG(LINFO) << "computet for "<<counter-1 <<" and "<< i << "  correspondences2: " << correspondences2->size() << " out of " << correspondences2->size();
	}
	CLOG(LINFO) << "view " << counter << " have correspondences with " << added << " views";
	if (added == 0 )
		CLOG(LINFO) << endl << " !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" <<endl;

	CLOG(LINFO) << "cloud_merged from LUM ";


	//	lum_sift.setCorrespondences(counter-1, i, pcl::CorrespondencesPtr(&inliers2));
	//	CLOG(LINFO) << "corr z 5 do 3 " << lum_sift.getCorresponences(5,3)->size();
	//lum_rgb.compute();
//	if (counter > 150) {
//		lum_sift.setMaxIterations(10);
//		lum_sift.compute();
//		CLOG(LINFO) << "ended";
//		CLOG(LINFO) << "cloud_merged from LUM ";
//		*cloud_merged = *(rgb_views[0]);
//		for (int i = 1 ; i < counter; i++)
//		{
//			pcl::PointCloud<pcl::PointXYZRGB> tmp = *(rgb_views[i]);
//			pcl::transformPointCloud(tmp, tmp, lum_sift.getTransformation (i));
//			*cloud_merged += tmp;
//		}
//	}

	*cloud_merged = *(rgb_views[0]);
	for (int i = 1 ; i < counter; i++)
	{
		pcl::PointCloud<pcl::PointXYZRGB> tmp = *(rgb_views[i]);
		pcl::transformPointCloud(tmp, tmp, lum_sift.getTransformation (i));
		*cloud_merged += tmp;
	}

	cloud_sift_merged = lum_sift.getConcatenatedCloud ();


		//*cloud_sift_merged += *cloud_sift;
//
	CLOG(LINFO) << "model cloud->size(): "<< cloud_merged->size();
	CLOG(LINFO) << "model cloud_sift->size(): "<< cloud_sift_merged->size();


	// Compute mean number of features.
	mean_viewpoint_features_number = total_viewpoint_features_number/counter;

	// Push results to output data ports.
	out_mean_viewpoint_features_number.write(mean_viewpoint_features_number);
	out_cloud_xyzrgb.write(cloud_merged);
	out_cloud_xyzsift.write(cloud_sift_merged);

	// Push SOM - depricated.
//	out_instance.write(produce());
}

void LUMGenerator::out_trigger(){

}


} //: namespace LUMGenerator
} //: namespace Processors
