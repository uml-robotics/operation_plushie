#include "FindBowl.cpp"

int 
main (int argc, char** argv)
{
    ros::init (argc, argv, "find_bowl_node");
    FindBowl fb;
    fb.begin_detection();   
    return (0);
}
