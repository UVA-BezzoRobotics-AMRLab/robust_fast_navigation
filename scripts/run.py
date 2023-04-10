import time
import argparse
import subprocess
import os
from os.path import join

import numpy as np
import rospy
import rospkg

from gazebo_simulation import GazeboSimulation

INIT_POSITION = [-2, 3, 1.57]  # in world frame
GOAL_POSITION = [8.5,0]  # relative to the initial position

gazebo_process = None
nav_stack_process = None
planner_process = None

def compute_distance(p1, p2):
    return ((p1[0] - p2[0]) ** 2 + (p1[1] - p2[1]) ** 2) ** 0.5

def shutdown():
    global gazebo_process, nav_stack_process, planner_process
    
    rospy.loginfo("*************** shutting nodes down now! ***************")
    gazebo_process.terminate()
    gazebo_process.wait()
    nav_stack_process.terminate()
    nav_stack_process.wait()
    planner_process.terminate()
    planner_process.wait()

def path_coord_to_gazebo_coord(x, y):
        RADIUS = 0.075
        r_shift = -RADIUS - (30 * RADIUS * 2)
        c_shift = RADIUS + 5

        gazebo_x = x * (RADIUS * 2) + r_shift
        gazebo_y = y * (RADIUS * 2) + c_shift

        return (gazebo_x, gazebo_y)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description = 'test BARN navigation challenge')
    parser.add_argument('--world_idx', type=int, default=0)
    parser.add_argument('--gui', action="store_true")
    parser.add_argument('--out', type=str, default="out.txt")
    args = parser.parse_args()
    
    ##########################################################################################
    ## 0. Launch Gazebo Simulation
    ##########################################################################################
    
    # os.environ["JACKAL_LASER"] = "1"
    # os.environ["JACKAL_LASER_MODEL"] = "ust10"
    # os.environ["JACKAL_LASER_OFFSET"] = "-0.065 0 0.01"
    
    rospack = rospkg.RosPack()
    base_path = rospack.get_path('jackal_gazebo')
    world_name = "BARN/world_%d.world" %(args.world_idx)
    print(">>>>>>>>>>>>>>>>>> Loading Gazebo Simulation with %s <<<<<<<<<<<<<<<<<<" %(world_name))   
    
    launch_file = join(base_path, 'launch', 'barn_world.launch')
    world_name = join(base_path, "worlds", world_name)

    gazebo_process = subprocess.Popen([
        'roslaunch',
        launch_file,
        'world_name:=' + world_name,
        'gui:=' + ("true" if args.gui else "false")
    ])
    time.sleep(5)  # sleep to wait until the gazebo being created
    
    rospy.init_node('gym', anonymous=True) #, log_level=rospy.FATAL)
    rospy.set_param('/use_sim_time', True)
    rospy.on_shutdown(shutdown)
    
    # GazeboSimulation provides useful interface to communicate with gazebo  
    gazebo_sim = GazeboSimulation(init_position=INIT_POSITION)
    
    init_coor = (INIT_POSITION[0], INIT_POSITION[1])
    goal_coor = (INIT_POSITION[0] + GOAL_POSITION[0], INIT_POSITION[1] + GOAL_POSITION[1])
    
    pos = gazebo_sim.get_model_state().pose.position
    curr_coor = (pos.x, pos.y)
    collided = True
    
    # check whether the robot is reset, the collision is False
    while compute_distance(init_coor, curr_coor) > 0.1 or collided:
        gazebo_sim.reset() # Reset to the initial position
        pos = gazebo_sim.get_model_state().pose.position
        curr_coor = (pos.x, pos.y)
        collided = gazebo_sim.get_hard_collision()
        time.sleep(1)




    ##########################################################################################
    ## 1. Launch your navigation stack
    ## (Customize this block to add your own navigation stack)
    ##########################################################################################
    
    time.sleep(5)
    planner_path = rospack.get_path('robust_fast_navigation')
    nav_stack_launch_file = join(planner_path, 'launch/navigation_stack.launch')
    nav_stack_process = subprocess.Popen([
        'roslaunch',
        nav_stack_launch_file,
    ])
    time.sleep(1)

    planner_launch_file = join(planner_path, 'launch/planner.launch')
    planner_process = subprocess.Popen([
        'roslaunch',
        planner_launch_file,
    ])
    
    # Make sure your navigation stack recives a goal of (0, 10, 0), which is 10 meters away
    # along postive y-axis.
    
    from geometry_msgs.msg import PoseStamped, Quaternion

    goal_pub = rospy.Publisher("/gap_goal", PoseStamped, queue_size=1, latch=True)
    mb_goal = PoseStamped()
    mb_goal.header.frame_id = 'map'
    mb_goal.header.stamp = rospy.Time.now()
    mb_goal.pose.position.x = GOAL_POSITION[0]
    mb_goal.pose.position.y = GOAL_POSITION[1]
    mb_goal.pose.position.z = 0
    mb_goal.pose.orientation = Quaternion(0, 0, 0, 1)

    goal_pub.publish(mb_goal)



    ##########################################################################################
    ## 2. Start navigation
    ##########################################################################################
    
    curr_time = rospy.get_time()
    pos = gazebo_sim.get_model_state().pose.position
    curr_coor = (pos.x, pos.y)

    
    # check whether the robot started to move
    while compute_distance(init_coor, curr_coor) < 0.1 or planner_process.poll() is not None:
        curr_time = rospy.get_time()
        pos = gazebo_sim.get_model_state().pose.position
        curr_coor = (pos.x, pos.y)
        time.sleep(0.01)
    
    # start navigation, check position, time and collision
    start_time = curr_time
    start_time_cpu = time.time()
    collided = False

    if planner_process.poll() is not None:
        collided = True
    
    while compute_distance(goal_coor, curr_coor) > 1 and not collided and curr_time - start_time < 70 and curr_coor[1] < 10:
        print(curr_coor, curr_coor[1] < 10)
        curr_time = rospy.get_time()
        pos = gazebo_sim.get_model_state().pose.position
        curr_coor = (pos.x, pos.y)
        print("Time: %.2f (s), x: %.2f (m), y: %.2f (m)" %(curr_time - start_time, *curr_coor), end="\r")
        collided = gazebo_sim.get_hard_collision()
        while rospy.get_time() - curr_time < 0.1:
            time.sleep(0.01)
    
    
    ##########################################################################################
    ## 3. Report metrics and generate log
    ##########################################################################################
    
    print(">>>>>>>>>>>>>>>>>> Test finished! <<<<<<<<<<<<<<<<<<")
    success = False
    if collided:
        status = "collided"
    elif curr_time - start_time >= 70:
        status = "timeout"
    else:
        status = "succeeded"
        success = True
    print("Navigation %s with time %.4f (s)" %(status, curr_time - start_time))
    
    path_file_name = join(base_path, "worlds/BARN/path_files", "path_%d.npy" %args.world_idx)
    path_array = np.load(path_file_name)
    path_array = [path_coord_to_gazebo_coord(*p) for p in path_array]
    path_array = np.insert(path_array, 0, (INIT_POSITION[0], INIT_POSITION[1]), axis=0)
    path_array = np.insert(path_array, len(path_array), (INIT_POSITION[0] + GOAL_POSITION[0], INIT_POSITION[1] + GOAL_POSITION[1]), axis=0)
    path_length = 0
    for p1, p2 in zip(path_array[:-1], path_array[1:]):
        path_length += compute_distance(p1, p2)
    
    # Navigation metric: 1_success *  optimal_time / clip(actual_time, 4 * optimal_time, 8 * optimal_time)
    optimal_time = path_length / 2
    actual_time = curr_time - start_time
    nav_metric = int(success) * optimal_time / np.clip(actual_time, 4 * optimal_time, 8 * optimal_time)
    print("Navigation metric: %.4f" %(nav_metric))
    
    with open(args.out, "a") as f:
        f.write("%d %d %d %d %.4f %.4f\n" %(args.world_idx, success, collided, (curr_time - start_time)>=100, curr_time - start_time, nav_metric))
    
    gazebo_process.terminate()
    gazebo_process.wait()
    nav_stack_process.terminate()
    nav_stack_process.wait()
    planner_process.terminate()
    planner_process.wait()