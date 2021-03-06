/* This program calls the IK Position Solver service used for moving the arm to desired coordinates. */

#include "RepositionHand.h"

const int RepositionHand::RIGHT = 0;
const int RepositionHand::LEFT = 1;

RepositionHand::RepositionHand()
{
    reposition_hand_service = n.advertiseService("reposition_hand_service", &RepositionHand::callback, this);
    reposition_progress_service = n.advertiseService("reposition_progress_service", &RepositionHand::progressCallback, this);    

    joint_pub[RIGHT] = n.advertise<baxter_core_msgs::JointCommand>("/robot/limb/right/joint_command", 200);
    joint_pub[LEFT] = n.advertise<baxter_core_msgs::JointCommand>("/robot/limb/left/joint_command", 200);

    endstate_sub[RIGHT] = n.subscribe<baxter_core_msgs::EndpointState>("/robot/limb/right/endpoint_state", 1000, &RepositionHand::updateRightEndpoint, this);
    endstate_sub[LEFT] = n.subscribe<baxter_core_msgs::EndpointState>("/robot/limb/left/endpoint_state", 1000, &RepositionHand::updateLeftEndpoint, this);
    
    torque_sub = n.subscribe<sensor_msgs::JointState>("/robot/joint_states", 1000, &RepositionHand::updateEffort, this);
    isMoving = false;
}

/* Sets up the message for the desired position and calls the IK Solver service, using returned info to see if the position is possible. */
bool 
RepositionHand::callback(operation_plushie::RepositionHand::Request &req, operation_plushie::RepositionHand::Response &res)
{
    isLeft = req.isLeft;
    needsConsistency = req.needsConsistency;

    ik_solver = n.serviceClient<baxter_core_msgs::SolvePositionIK>(
        std::string("/ExternalTools/") + (isLeft?"left":"right") + "/PositionKinematicsNode/IKService");
    
    baxter_core_msgs::SolvePositionIK srv;

    //The following math functions take a Roll, Pitch and Yaw and convert them to the XYZW for orientation.
    //These Roll, Pitch and Yaw values correspond to the hand pointing straight down.
    const double ROLL = 0, PITCH = 3.14, YAW = req.yaw;

    double mathc1 = cos(PITCH),
        maths1 = sin(PITCH),
        mathc2 = cos(YAW),
        maths2 = sin(YAW),
        mathc3 = cos(ROLL),
        maths3 = sin(ROLL),
        oriw, oriw4, orix, oriy, oriz;
        
    oriw = sqrt(1.0 + mathc1 * mathc2 + mathc1 * mathc3 
        - maths1 * maths2 * maths3 + mathc2 * mathc3) / 2.0;
    oriw4 = (4.0 * oriw);
    orix = (mathc2 * maths3 + mathc1 * maths3 + maths1 * maths2 * mathc3) / oriw4;
    oriy = (maths1 * mathc2 + maths1 * mathc3 + mathc1 * maths2 * maths3) / oriw4;
    oriz = (-maths1 * maths3 + mathc1 * maths2 * mathc3 + maths2) / oriw4;

    //This time stamp should be set to current time.
    ps.header.frame_id = req.frame; 
    ps.header.stamp = ros::Time::now();

    //Sets the XYZ position of the desired pose to the XYZ values from the request.
    //The x-value should not be greater than .84 or it'll be too far for Baxter to reach.
    ps.pose.position.x = (req.x > .84 ? .84 : req.x);
    ps.pose.position.y = req.y;
    ps.pose.position.z = req.z;

    //XYZW of orientation is set to the calculated values for each.
    ps.pose.orientation.x = orix;
    ps.pose.orientation.y = oriy;
    ps.pose.orientation.z = oriz;
    ps.pose.orientation.w = oriw;
    //end math functions

    srv.request.pose_stamp.push_back(ps);

    if(!ik_solver.call(srv))
    {
        ROS_ERROR("Failed to call service IKSolver");
        return false;
    }

    if(!srv.response.isValid[0])
    {
        ROS_ERROR("Not a valid position, :(");
        isStuck = true;
        return true;
    }
   
    //Sets arm motion to position mode.
    msg.mode = 1;
    
    for(int i = 0; i < srv.response.joints[0].name.size(); i++)
    {
        msg.names.push_back(srv.response.joints[0].name[i]);
        msg.command.push_back(srv.response.joints[0].position[i]);
    }

    s1_torque_original = s1_torque;
    isMoving = true;
    isStuck = false;

    return true;
}

/* Updates the position of Baxter's left hand. */
void 
RepositionHand::updateLeftEndpoint(baxter_core_msgs::EndpointState eps)
{
    if(isLeft)
        updateEndpoint(eps);
}

/* Updates the position of Baxter's right hand. */
void 
RepositionHand::updateRightEndpoint(baxter_core_msgs::EndpointState eps)
{
    if(!isLeft)
        updateEndpoint(eps);
}

/* Checks the position and orientation of desired endpoint. Checks if the hand gets stuck while it's trying to move. */
void 
RepositionHand::updateEndpoint(baxter_core_msgs::EndpointState eps)
{
    const double P_WIG = .001f, O_WIG = .05f;
    if(fabs(eps.pose.position.x - c_x) < P_WIG && fabs(eps.pose.position.y - c_y) < P_WIG && fabs(eps.pose.position.z - c_z) < P_WIG 
        && fabs(eps.pose.orientation.x - c_orix) < O_WIG && fabs(eps.pose.orientation.y - c_oriy) < O_WIG && fabs(eps.pose.orientation.z - c_oriz) < O_WIG && fabs(eps.pose.orientation.w - c_oriw) < O_WIG)
    {
        c_pose_count++;
    }
    else
    {
        c_pose_count = 0;
        c_x = eps.pose.position.x;
        c_y = eps.pose.position.y;
        c_z = eps.pose.position.z;
        c_orix = eps.pose.orientation.x;
        c_oriy = eps.pose.orientation.y;
        c_oriz = eps.pose.orientation.z;
        c_oriw = eps.pose.orientation.w;
    }
    
    if(isMoving)
    {
        if(isPositioned(eps)) {
            isMoving = false;
            return;
        }

        //ROS_INFO("consistent_torque_count: %d, c_pose_count: %d", consistent_torque_count, c_pose_count); 
        
        joint_pub[(isLeft ? LEFT : RIGHT)].publish(msg);        
        
        //If the hand can't move, reassign variables to reflect that Baxter's hand is stuck.
        if(needsConsistency && consistent_torque_count > 15 && c_pose_count > 15 && ps.pose.position.z < eps.pose.position.z)
        {
            ROS_INFO("final torque: %f", s1_torque);
            isMoving = false;
            isStuck = true;
        }
    }
}

/* Check if the hand is in the correct position. Return true if this is true. */
bool 
RepositionHand::isPositioned(baxter_core_msgs::EndpointState eps)
{
    const double P_WIG = .007f, O_WIG = .05f;
    
    return (fabs(eps.pose.position.x - ps.pose.position.x) < P_WIG && fabs(eps.pose.position.y - ps.pose.position.y) < P_WIG && fabs(eps.pose.position.z - ps.pose.position.z) < P_WIG 
        && fabs(eps.pose.orientation.x - ps.pose.orientation.x) < O_WIG && fabs(eps.pose.orientation.y - ps.pose.orientation.y) < O_WIG && fabs(eps.pose.orientation.z - ps.pose.orientation.z) < O_WIG && fabs(eps.pose.orientation.w - ps.pose.orientation.w) < O_WIG);

}

/* Updates the value of the shoulder's torque and checks if it's been at a consistent value. */
void 
RepositionHand::updateEffort(sensor_msgs::JointState js)
{
    int indexSpacing = (isLeft ? 0 : 7);

    s1_torque = js.effort[(isLeft ? 5 : 12)];
    if(fabs(s1_torque - consistent_torque) < .15)
    {
        consistent_torque_count++;
    }
    else
    {
        consistent_torque = s1_torque;
        consistent_torque_count = 0;
    }
}

/* Updates variables that reflect the progress of the motion. */
bool 
RepositionHand::progressCallback(operation_plushie::isComplete::Request &req, operation_plushie::isComplete::Response &res)
{
    res.isComplete = !isMoving;
    res.isStuck = isStuck;
    return true;
}
