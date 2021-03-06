/* This program is used to deliver the plush toy to the person by stretching its arm out in the direction that its head is facing. It will then return to its original pose. */

#include "Delivery.h"

//Assumed order of joint positions: e0 e1 s0 s1 w0 w1 w2

/*instantiates ros objects*/
Delivery::Delivery() : loop_rate(10) {
    armState = n.subscribe<sensor_msgs::JointState>("/robot/joint_states", 1000, &Delivery::callback, this);
    deliveryService = n.advertiseService("delivery_service", &Delivery::deliver, this);
    isComplete_service = n.advertiseService("delivery_isComplete_service", &Delivery::isComplete, this);
    
    state = FINISHED;
}

/* Basically the heart of the delivery node. */
void 
Delivery::callback(sensor_msgs::JointState msg)
{
    //will not make a delivery until activated by a serviceClient
    if(state == FINISHED)
        return;

    //once past the if, Baxter will make a delivery
    storeJointStates(msg); 

    selectState();
}

void 
Delivery::storeJointStates(sensor_msgs::JointState msg)
{
    int left_e0Index = -999;
    //Search for "left_e0" in the array of joint names.
    for(int i = 0; i < msg.name.size(); i++)
    {
        if(strcmp(msg.name[i].c_str(), "left_e0") == 0)
        {
            left_e0Index = i;
            break;
        }
    }

    //If left_e0Index is still the rogue value, then it didn't find left_e0.
    if(left_e0Index == -999)
    {
        ROS_ERROR("left_e0 not found.\n");
        exit(1);
    }

    /* WARNING: assumes there are 7 left joints before the right joints in the msg.position vector
       we are finding the joints from the JointState msg through some assumption */
    int index_space = (isLeft ? 0 : 7);
    
    std::vector<double>::const_iterator first = msg.position.begin() + left_e0Index + index_space;
    std::vector<double>::const_iterator last = msg.position.begin() + left_e0Index + index_space + 7;
    current_arm_positions = std::vector<double>(first, last);

    //stores the original pose before moving to the delivery position 
    if(!origStored)
    {
        origPose.command = current_arm_positions;
    }
}

/* With the 'state' instance variable, we select which task is next in the delivery process using enums */
void 
Delivery::selectState()
{    
    switch(state)
    {
        case STRETCHING:
            stretch();
            break;
        case RELEASING:
            release();
            break;
        case RETURNING:
            returning();
            break;
    }
}


/* The function that begins the delivery process */
bool 
Delivery::deliver(operation_plushie::Deliver::Request &req, operation_plushie::Deliver::Response &res)
{
    isLeft = req.isLeft;
    
    //Perhaps bad practice, we create ros objects for each delivery to allow for left and right arms to be used
    gripper_pub = n.advertise<baxter_core_msgs::EndEffectorCommand>(
        std::string("/robot/end_effector/") + (isLeft ? "left" : "right") + "_gripper/command", 1000);

    button_sub = n.subscribe<baxter_core_msgs::DigitalIOState>(
        std::string("/robot/digital_io/") + (isLeft ? "left" : "right") + "_itb_button0/state", 10, &Delivery::updateButtonState, this);

    is_holding_sub = n.subscribe<baxter_core_msgs::EndEffectorState>(
        std::string("/robot/end_effector/") + (isLeft ? "left" : "right") + "_gripper/state", 10, &Delivery::updateEndEffectorState, this);

    //empty any old values left in the vector
    stretchPose.names.clear();
    origPose.names.clear();
    stretchPose.command.clear();
    origPose.command.clear();
 
    //Fills up the names array that corresponds in a dictionary fashion with the command vector.
    if(isLeft)
    {
        armPose_pub = n.advertise<baxter_core_msgs::JointCommand>("/robot/limb/left/joint_command", 1000);

        stretchPose.names.push_back("left_e0");
        stretchPose.names.push_back("left_e1");
        stretchPose.names.push_back("left_s0");
        stretchPose.names.push_back("left_s1");
        stretchPose.names.push_back("left_w0");
        stretchPose.names.push_back("left_w1");
        stretchPose.names.push_back("left_w2");
    }
    else 
    {
        armPose_pub = n.advertise<baxter_core_msgs::JointCommand>("/robot/limb/right/joint_command", 1000);

        stretchPose.names.push_back("right_e0");
        stretchPose.names.push_back("right_e1");
        stretchPose.names.push_back("right_s0");
        stretchPose.names.push_back("right_s1");
        stretchPose.names.push_back("right_w0");
        stretchPose.names.push_back("right_w1");
        stretchPose.names.push_back("right_w2");
    }    
    
    origPose.names = stretchPose.names;

    //Fill up the command vector for stretchPose.
    //These commands move the arm into a outstretched position
    stretchPose.command.push_back( (isLeft ? -1.5 : 1.5) );
    stretchPose.command.push_back(0.2);
    stretchPose.command.push_back(getArmPos(req.headPos));
    stretchPose.command.push_back(0);
    stretchPose.command.push_back( (isLeft ? 1.5 : -1.5) );
    stretchPose.command.push_back(0.3);
    stretchPose.command.push_back(0);

    //Sets both poses to position mode.
    stretchPose.mode = 1; 
    origPose.mode = 1;
   
    origStored = false;
    isPressed = false;

    //the next callback function will call stretch()
    state = STRETCHING;

    return true;
}

/* Calculates where the arm should be relative to the head pan */
double 
Delivery::getArmPos(double headPos)
{
    if(isLeft && headPos < 0)
        return -0.8;
    else if(!isLeft && headPos > 0)
        return 0.8;
    else
        return headPos + (isLeft ? -0.8 : 0.8);
}

/* Moves arm into the outstretched pose until it is positioned */
void 
Delivery::stretch()
{
    if(isCorrectPosition(stretchPose))
        state = RELEASING;
    else
        armPose_pub.publish(stretchPose);
}

/* Baxter waits in this stage until he is either not holding the object, or his arm button is pressed 
   he then releases his grip */
void 
Delivery::release()
{
    if(!isHolding || isPressed)
    {
        baxter_core_msgs::EndEffectorCommand hand_command;
        hand_command.id = 65538;
        hand_command.command = "release";
        gripper_pub.publish(hand_command);

        usleep(1000000);

        state = RETURNING;
    }
}

/* Same concept as stretch() */
void 
Delivery::returning()
{
    if(isCorrectPosition(origPose))
        state = FINISHED;
    else
        armPose_pub.publish(origPose);
}

/* determines if the robot's current state matches the JointCommand 'msg' */
bool 
Delivery::isCorrectPosition(baxter_core_msgs::JointCommand msg)
{
    for(int i = 0; i < current_arm_positions.size(); i++) 
    {
        if(fabs(current_arm_positions[i] - msg.command[i]) > 0.1)
            return false;
    }

    return true;
}

/* Constantly checking button state. Sets isPressed permanently to true if the button is pressed (permanence for each delivery) */
void 
Delivery::updateButtonState(baxter_core_msgs::DigitalIOState ok)
{
    if(ok.state)
        isPressed = true;
}

/* Sevice that updates other nodes with the progress of the delivery
   Once the delivery is complete other nodes can move to the next step e.g. face tracking, or picking up a plushie, etc. */
bool 
Delivery::isComplete(operation_plushie::isComplete::Request &req, operation_plushie::isComplete::Response &res) 
{
    res.isComplete = (state == FINISHED);
    return true;
}

/* Simple function to check if Baxter is gripping an object */
void 
Delivery::updateEndEffectorState(baxter_core_msgs::EndEffectorState ees)
{
    isHolding = ees.gripping;
}

