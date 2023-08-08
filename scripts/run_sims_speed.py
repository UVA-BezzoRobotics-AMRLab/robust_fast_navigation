import sys
import math
import time
import argparse
import subprocess
import os
from os.path import join

import numpy as np
import rospy
import rospkg

from gazebo_simulation import GazeboSimulation

POSITIONS=np.array([
    [0,0,0],
    [8.0,6.0,0],
])


bag_process = None
gazebo_process = None
nav_stack_process = None
planner_process = None

def compute_distance(p1, p2):
    return ((p1[0] - p2[0]) ** 2 + (p1[1] - p2[1]) ** 2) ** 0.5

def shutdown():
    global gazebo_process, nav_stack_process, planner_process
    
    rospy.loginfo("*************** shutting nodes down now! ***************")
    if bag_process != None:
        bag_process.terminate()
        bag_process.wait()

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
    parser.add_argument('--solver', type=int, default=0)
    parser.add_argument('--world_idx', type=int, default=0)
    parser.add_argument('--gui', action="store_true")
    parser.add_argument('--out', type=str, default="out.txt")
    parser.add_argument('--bag', action="store_true")
    parser.add_argument('--max_speed', type=float, default=1.0)
    args = parser.parse_args()
    
    ##########################################################################################
    ## 0. Launch Gazebo Simulation
    ##########################################################################################
    
    os.environ["JACKAL_LASER"] = "1"
    os.environ["JACKAL_LASER_MODEL"] = "ust10"
    os.environ["JACKAL_LASER_OFFSET"] = "-0.065 0 0.01"

    rospack = rospkg.RosPack()
    base_path = rospack.get_path('jackal_helper')
    world_path = rospack.get_path('jackal_gazebo')
    world_name = "occluded_trap.world"
    print(">>>>>>>>>>>>>>>>>> Loading Gazebo Simulation with %s <<<<<<<<<<<<<<<<<<" %(world_name))   
    
    launch_file = join(base_path, 'launch', 'gazebo_launch.launch')
    world_name = join(world_path, "worlds", world_name)

    INIT_POSITION = POSITIONS[0]
    GOAL_POSITION = POSITIONS[1]

    heading = math.atan2(-INIT_POSITION[1]+GOAL_POSITION[1], -INIT_POSITION[0]+GOAL_POSITION[0])
    INIT_POSITION[2] = heading

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
    # due to map / world transform, flip goal_pos coords...
    goal_coor = (GOAL_POSITION[0], GOAL_POSITION[1])
    
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
    
    planner_path = rospack.get_path('robust_fast_navigation')

    bag_dir_path = None
    bag_fname = None

    if args.bag:

        from datetime import datetime

        bag_dir_path = os.path.join(planner_path, 'bags')
        dt_str = "{:%Y_%m_%d-%H-%M-%S}".format(datetime.now())
        bag_fname= f'occluded_trap_{args.max_speed}_{dt_str}'
        bag_launch_file = os.path.join(planner_path, 'launch/record_bag.launch')
        
        if not os.path.exists(bag_dir_path):
            os.makedirs(bag_dir_path)

        # i = 0
        # MAX_ITER=1000
        # bag_fname = init_bag_fname+"%d.bag" %(i)
        # bag_path_name=os.path.join(bag_dir_path, bag_fname)

        # while i < MAX_ITER and os.path.exists(bag_path_name):
        #     i += 1
        #     bag_fname = init_bag_fname+"%d.bag" %(i)
        #     bag_path_name=os.path.join(bag_dir_path, bag_fname)


        print("opening bag file at", (os.path.join(bag_dir_path,bag_fname)))

        bag_process = subprocess.Popen([
            'roslaunch',
            bag_launch_file,
            'path:=' + bag_dir_path,
            'bag_name:=' + bag_fname,
        ])

    time.sleep(5)
    nav_stack_launch_file = join(planner_path, 'launch/navigation_stack.launch')
    nav_stack_process = subprocess.Popen([
        'roslaunch',
        nav_stack_launch_file,
    ])
    time.sleep(3)

    init_xy = np.array(INIT_POSITION[:2])
    final_xy = np.array(GOAL_POSITION[:2])

    planner_launch_file = join(planner_path, 'launch/planner.launch') if args.solver == 1 else join(planner_path, 'launch/planner_gurobi.launch')
    planner_process = subprocess.Popen([
        'roslaunch',
        planner_launch_file,
        'barn:= true', 
        'barn_dist:=' + str(np.linalg.norm(final_xy-init_xy)),
        'max_vel:=' + str(args.max_speed)
    ])
    
    # Make sure your navigation stack recives a goal of (0, 10, 0), which is 10 meters away
    # along postive y-axis.
    
    from geometry_msgs.msg import PoseStamped, Quaternion

    goal_pub = rospy.Publisher("/final_goal", PoseStamped, queue_size=1, latch=True)
    mb_goal = PoseStamped()
    mb_goal.header.frame_id = 'odom'
    mb_goal.header.stamp = rospy.Time.now()
    mb_goal.pose.position.x = GOAL_POSITION[0]
    mb_goal.pose.position.y = GOAL_POSITION[1]
    mb_goal.pose.position.z = 0
    mb_goal.pose.orientation = Quaternion(0, 0, 0, 1)

    # goal_pub.publish(mb_goal)


    ##########################################################################################
    ## 2. Start navigation
    ##########################################################################################
    
    curr_time = rospy.get_time()
    pos = gazebo_sim.get_model_state().pose.position
    curr_coor = (pos.x, pos.y)

    prog_crash = False
    start_time = curr_time
    # check whether the robot started to move
    while compute_distance(init_coor, curr_coor) < 0.1 or planner_process.poll() is not None:

        if curr_time - start_time > 60:
            prog_crash = True
            break

        curr_time = rospy.get_time()
        pos = gazebo_sim.get_model_state().pose.position
        curr_coor = (pos.x, pos.y)
        time.sleep(0.01)
    
    # start navigation, check position, time and collision
    start_time = curr_time
    start_time_cpu = time.time()
    collided = False
    timeout_time = 10000

    if planner_process.poll() is not None or prog_crash:
        collided = True
    
    while compute_distance(goal_coor, curr_coor) > 2 and not collided and curr_time - start_time < timeout_time:
        print(f"DISTANCE IS {compute_distance(goal_coor, curr_coor)}")
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
    elif curr_time - start_time >= timeout_time:
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
        f.write("%d %d %d %d %.4f %.4f\n" %(args.max_speed, success, collided, (curr_time - start_time)>=100, curr_time - start_time, nav_metric))
    
    if bag_process != None:
        bag_process.terminate()
        bag_process.wait()
    
    # if args.bag and nav_metric >= 0:
    #     print("Navigation succeeded, so deleting bag file")
    #     os.remove(os.path.join(bag_dir_path,bag_fname))


    gazebo_process.terminate()
    gazebo_process.wait()
    nav_stack_process.terminate()
    nav_stack_process.wait()
    planner_process.terminate()
    planner_process.wait()