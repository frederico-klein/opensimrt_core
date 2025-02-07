#include "common_node.h"
#include "ros/message_traits.h"
#include "ros/ros.h"
#include "opensimrt_msgs/CommonTimed.h"
#include "opensimrt_msgs/Labels.h"
#include "experimental/AccelerationBasedPhaseDetector.h"
#include "experimental/GRFMPrediction.h"
#include "INIReader.h"
#include "OpenSimUtils.h"
//#include "Settings.h"
#include "SignalProcessing.h"
#include "Utils.h"
#include "Visualization.h"
#include <Actuators/Thelen2003Muscle.h>
#include <Common/TimeSeriesTable.h>
#include <OpenSim/Common/STOFileAdapter.h>
#include <OpenSim/Common/CSVFileAdapter.h>
#include <exception>
#include <numeric>
#include <utility>
#include <vector>
#include "ros/service_server.h"
#include "ros/time.h"
#include "signal.h"
#include "std_srvs/Empty.h"
#include "Pipeline/include/grf_pipe.h"

using namespace std;
using namespace OpenSim;
using namespace SimTK;
using namespace OpenSimRT;

Pipeline::Grf::Grf()
{
	//TODO: Move common things here
	ROS_WARN_STREAM("not implemented");
}

Pipeline::Grf::~Grf()
{
	ROS_INFO_STREAM("Shutting down Grf");
}

void Pipeline::Grf::onInit() {
	Pipeline::CommonNode::onInit();
	previousTime = ros::Time::now().toSec();
	previousTimeDifference = 0;

	// when i am running this it is already initialized, so i have to add the loggers to the list I want to save afterwards
	initializeLoggers("grfRight",grfRightLogger);
	initializeLoggers("grfLeft", grfLeftLogger);
	initializeLoggers("tau",tauLogger);

}

void Pipeline::Grf::callback(const opensimrt_msgs::CommonTimedConstPtr& message) {
//void Pipeline::Grf::operator() (const opensimrt_msgs::CommonTimedConstPtr& message) {
// repeat the simulation `simulationLoops` times
	ROS_DEBUG_STREAM("Received message. Running Grf loop"); 
	counter++;
	//for (int k = 0; k < qTable.getNumRows() * simulationLoops; k++) {
	
	// well yes, but actually no.
	// get raw pose from table
	//auto qRaw_old = qTable.getRowAtIndex(i).getAsVector();
	//qRaw_old( qRaw_old +"dddd" +1);
	//std::vector<double> sqRaw = std::vector<double>(message->data.begin() + 1, message->data.end());
	SimTK::Vector qRaw(19); //cant find the right copy constructor syntax. will for loop it
	for (int j = 0;j < qRaw.size();j++)
	{
		//qRaw[j] = sqRaw[j];
		qRaw[j] = message->data[j];
	}
	


	//is it the same
	//if (qRaw.size() != qRaw_old.size())
	//	ROS_FATAL("size is different!");
	//OpenSim::TimeSeriesTable i
	//get_from_subscriber(qRaw,t); //this will set qRaw and t from the subscribert

	//double t_old = qTable.getIndependentColumn()[i];
	
	/*
	 * This is kinda important. The time that matters is the time of the acquisition, I think
	 * The variable time it takes to calculate it doesn't matter too much, UNLESS, it is too big,
	 * then we should probably forget about it and not try to calculate anything!
	 * */
	//NO! I AM NOT SENDING TIME LIKE THIS ANYMORE.
	//double t = message->header.stamp.toSec();
	double t = message->time;
	double timediff = t- previousTime;
	double ddt = timediff-previousTimeDifference;
	if (std::abs(ddt) > 1e-5 )
		ROS_WARN_STREAM("Time difference greater than what our filter can handle: "<< std::setprecision(7) << ddt ); 
	previousTime = t;
	previousTimeDifference = timediff;
	ROS_DEBUG_STREAM("T (msg):"<< std::setprecision (15) << t);
	ROS_DEBUG_STREAM("DeltaT :"<< std::setprecision (15) << t);

	/*if (t_old - t > 0.1)
		ROS_ERROR("Reading from different timestamp! Did I lose a frame");
	else // same timestamp, so we check the indexes are okay.
	{
		for (int it=0; it < qRaw.size(); it++)
		{
			if (qRaw[it] - qRaw_old[it] > 0.1)
				ROS_ERROR("Difference too big");
		}
			
	}
*/
	//	ROS_DEBUG_STREAM("TAN" << qRaw);

	// increment the time by the total simulation time plus the sampling
	// period, to keep increasing after each simulation loop
//			t += loopCounter * (qTable.getIndependentColumn().back() + 0.01);

	// filter
	auto ikFiltered = filter->filter({t, qRaw});
	auto q = ikFiltered.x;
	auto qDot = ikFiltered.xDot;
	auto qDDot = ikFiltered.xDDot;
	ROS_DEBUG_STREAM("Filter ran ok");
	// increment loop
/*			if (++i == qTable.getNumRows()) {
	    i = 0;
	    loopCounter++;
	}*/
	if (!ikFiltered.isValid) {
		ROS_DEBUG_STREAM("filter results are NOT valid");
		return; }
	ROS_DEBUG_STREAM("Filter results are valid");

	chrono::high_resolution_clock::time_point t1;
	t1 = chrono::high_resolution_clock::now();

		// perform grfm prediction
	detector->updDetector({ikFiltered.t, q, qDot, qDDot});
	ROS_DEBUG_STREAM("Update detector ok");
	auto grfmOutput = grfm->solve({ikFiltered.t, q, qDot, qDDot});
	ROS_DEBUG_STREAM("GRFM estimation ran ok");

	chrono::high_resolution_clock::time_point t2;
	t2 = chrono::high_resolution_clock::now();
	sumDelayMS +=
		chrono::duration_cast<chrono::milliseconds>(t2 - t1).count();
	sumDelayMSCounter++;

	// project on plane
	grfmOutput.right.point =
		projectionOnPlane(grfmOutput.right.point, grfOrigin);
	grfmOutput.left.point =
		projectionOnPlane(grfmOutput.left.point, grfOrigin);

	// setup ID inputn
	ExternalWrench::Input grfRightWrench = {grfmOutput.right.point,
						grfmOutput.right.force,
						grfmOutput.right.torque};
	ExternalWrench::Input grfLeftWrench = {grfmOutput.left.point,
					       grfmOutput.left.force,
					       grfmOutput.left.torque};

	ROS_DEBUG_STREAM("updated visuals ok");

	// solve ID
	auto idOutput = id->solve(
		{ikFiltered.t, q, qDot, qDDot,
		 vector<ExternalWrench::Input>{grfRightWrench, grfLeftWrench}});
	
	ROS_DEBUG_STREAM("inverse dynamics ran ok");

	// visualization
	try {
	visualizer->update(q);
	rightGRFDecorator->update(grfmOutput.right.point,
				  grfmOutput.right.force);
	leftGRFDecorator->update(grfmOutput.left.point, grfmOutput.left.force);
		ROS_DEBUG_STREAM("visualizer ran ok.");
	}
	catch (std::exception& e)
	{
		ROS_ERROR_STREAM("Error in visualizer. cannot show data!!!!!" <<std::endl << e.what());
	}

	//
	//OpenSim::TimeSeriesTable output;
	std::vector<double> p;
	auto a = grfmOutput.right.toVector() ;
	auto b = grfmOutput.left.toVector() ;

	p.insert(p.end(),a.begin(),a.end());
	p.insert(p.end(),b.begin(),b.end());
	//output.appendRow(grfmOutput.t,p);
	opensimrt_msgs::CommonTimed msg;
	if (false)
	{
		std_msgs::Header h;
		h.frame_id = "subject";
		h.stamp = ros::Time::now();
		msg.header = h;
	} else
	{
		msg.header = message->header; //will this break? it will be publishing messages in the past
	}
	msg.time = message->time;
	msg.data.insert(msg.data.end(), p.begin(),p.end());
	pub.publish(msg);

	try{
		// log data (use filter time to align with delay)
		if(false)
		{
			ROS_WARN_STREAM("THIS SHOULDNT BE RUNNING");
			grfRightLogger->appendRow(grfmOutput.t, ~grfmOutput.right.toVector());
			grfLeftLogger->appendRow(grfmOutput.t, ~grfmOutput.left.toVector());
			tauLogger->appendRow(ikFiltered.t, ~idOutput.tau);
			ROS_INFO_STREAM("Added data to loggers. "<< counter);
		}
	}
	catch (std::exception& e)
	{
		ROS_WARN_STREAM("Error while updating loggers, data will not be saved" <<std::endl << e.what());
	}
	//if (counter > 720)
	//	write_();
	//}
}	
void Pipeline::Grf::finish() {
    cout << "Mean delay: " << double(sumDelayMS) / sumDelayMSCounter << " ms"
	 << endl;

    // Relax tolerance because of floating point errors between target machines
    // (this fails on Windows).
    OpenSimUtils::compareTables(
	    *grfRightLogger,
	    TimeSeriesTable(subjectDir + "real_time/grfm_prediction/"
					 "acceleration_based/wrench_right.sto"),
	    1e-1);
    OpenSimUtils::compareTables(
	    *grfLeftLogger,
	    TimeSeriesTable(subjectDir + "real_time/grfm_prediction/"
					 "acceleration_based/wrench_left.sto"),
	    1e-1);
    OpenSimUtils::compareTables(
	    *tauLogger,
	    TimeSeriesTable(
		    subjectDir +
		    "real_time/grfm_prediction/acceleration_based/tau.sto"),
	    1e-1);

}
bool Pipeline::Grf::see(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res)
{
	std::stringstream ss;
	//vector<string> data = grfRightLogger.getTableMetaData().getKeys();
	//ss << "Data Retrieved: \n";
	std::copy(grfRightLogger->getTableMetaData().getKeys().begin(), grfRightLogger->getTableMetaData().getKeys().end(), std::ostream_iterator<string>(ss, " "));
	ss << std::endl;
	ROS_INFO_STREAM("grfRightLogger columns:" << ss.str());
	return true;
}
void Pipeline::Grf::write_() {
//			std::stringstream ss;
	//vector<string> data = grfRightLogger.getTableMetaData().getKeys();
	//ss << "Data Retrieved: \n";
//		  	std::copy(grfRightLogger.getTableMetaData().getKeys().begin(), grfRightLogger.getTableMetaData().getKeys().end(), std::ostream_iterator<string>(ss, " "));
//		  	ss << std::endl;
//			ROS_INFO_STREAM("grfRightLogger columns:" << ss.str());


/*	ROS_INFO_STREAM("I TRY WRITE sto");
	STOFileAdapter::write(grfRightLogger,"grfRight.sto");
	STOFileAdapter::write(grfLeftLogger,"grfLeft.sto");
	STOFileAdapter::write(tauLogger,"tau.sto");
	ROS_INFO_STREAM("I TRY WRITE csv");
	CSVFileAdapter::write(grfRightLogger,"grfRight.csv");
	CSVFileAdapter::write(grfLeftLogger,"grfLeft.csv");
	CSVFileAdapter::write(tauLogger,"tau.csv");
*/
	saveCsvs();
	saveStos();
	ROS_INFO_STREAM("i write");
}

