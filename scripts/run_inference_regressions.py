#!/usr/bin/env python

import os
import copy
import time
import math
import rospy
import joblib
import rospkg
import numpy as np

from matplotlib import pyplot as plt

#ROS
from robust_fast_navigation.msg import SolverStateArray
from std_msgs.msg import Float32MultiArray, MultiArrayDimension

states = None

# Callback 
def solverStatecb(data):
    global states
    states = data.states
    
    rospy.logerr("Received solver states")

def extract_polygons_from_pose_array(pose_array_msg):
    polygons = []
    current_polygon = []

    for pose in pose_array_msg.poses:
        if pose.orientation.x == 0.0 and pose.orientation.y == 0.0 and pose.orientation.z == 0.0 and pose.orientation.w == 0.0:
            # Delimiter detected, add the current_polygon to polygons list
            if current_polygon:
                polygons.append(copy.copy(np.array(current_polygon)))
                current_polygon = []
        else:
            # Extract the hyperplane equation coefficients from the orientation part of the pose
            a = pose.orientation.x
            b = pose.orientation.y
            c = pose.orientation.z
            d = pose.orientation.w

            current_polygon.append(np.array([a, b, c, d]))

    return polygons

def get_time_to_intersect_poly(init_pva, polygon):
    point = init_pva.positions[:2]
    vel = np.array(init_pva.velocities[:2])

    if vel[0] == 0 and vel[1] == 0:
        return 1e2

    dist=0
    best_dist = np.inf
    for hyperplane in polygon:

        if math.fabs(hyperplane[2]) >= 1e-3:
            continue

        a = hyperplane[0]
        b = hyperplane[1]
        d = hyperplane[3]

        # Compute the distance to the hyperplane
        speed = vel.dot(hyperplane[:2]/np.linalg.norm(hyperplane[:2]))
        if speed < 0:
            continue

        dist = (-a*point[0]-b*point[1]-d) / (a*vel[0]+b*vel[1])
        # Update the speed if necessary
        if dist < best_dist:
            best_dist = dist
            # dist = np.abs(a*point[0] + b*point[1] + d) / np.sqrt(a**2 + b**2)

    return best_dist

def extract_input_from_states(states):
    
    X = []
    for state in states:
        polygons = extract_polygons_from_pose_array(state.polys)
        t_to_intersect = get_time_to_intersect_poly(state.initialPVA, polygons[0])
        X.append([t_to_intersect, len(polygons)])

    return X

def main():
    global states

    # Get dataset
    rospack = rospkg.RosPack()

    model_path = rospack.get_path('robust_fast_navigation')
    model_path = os.path.join(model_path, 'bags', 'models')

    # get all files in model_path directory and sort alphabetically
    files = os.listdir(model_path)
    files.sort()

    # Load model for each corridor
    gp_models = {}
    for i in range(2,8):
        gp_models[i] = joblib.load(os.path.join(model_path, files[i-2]))

    # initialize ros node
    rospy.init_node('run_inference', anonymous=True)
    rate = rospy.Rate(10) # 1hz

    # Subscriber for SolverState message
    rospy.Subscriber("/candidateRecoveryPoints", SolverStateArray, solverStatecb)

    # initialize publisher for Float32MultiArray
    pub = rospy.Publisher('/inference', Float32MultiArray, queue_size=10, latch=True)

    while not rospy.is_shutdown():
        rate.sleep()

        if states == None:
            continue


        input_data = extract_input_from_states(states)
        rospy.logerr(input_data)

        predictions = []
        start = time.time()
        for data in input_data:
            gp_model = gp_models[data[1]]
            predictions.append(gp_model.predict(np.array(data[0]).reshape(-1,1))[0])

        end = time.time()
        rospy.logerr("Inference time: {}".format(end-start))

        # remove any nans
        predictions = np.array([x for x in predictions if np.isnan(x) == False])

        predictions = np.array(predictions)
        # rospy.logerr(predictions)

        # get output and package into Float32MultiArray message
        msg = Float32MultiArray()
        dim = MultiArrayDimension()
        dim.label = "predictions"
        dim.size = predictions.shape[0]
        dim.stride = 1

        msg.layout.dim.append(dim)
        msg.data = predictions

        # publish message
        pub.publish(msg)

        states = None


if __name__ == "__main__":
    main()
