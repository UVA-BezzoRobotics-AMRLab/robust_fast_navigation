#include <math.h>
#include <vector>
#include <string>
#include <tf/tf.h>

#include <robust_fast_navigation/JPS.h>
#include <robust_fast_navigation/spline.h>
#include <robust_fast_navigation/planner.h>
#include <robust_fast_navigation/corridor.h>

Planner::Planner(ros::NodeHandle& nh){

    nh.param("robust_planner/max_velocity", _max_vel, 1.0);
    nh.param("robust_planner/planner_frequency", _dt, .1);
    nh.param("robust_planner/teleop", _is_teleop, false);
    nh.param<std::string>("robust_planner/frame", _frame_str, "map");

    trajVizPub = 
        nh.advertise<nav_msgs::Path>("/MINCO_path", 0);
    wptVizPub = 
        nh.advertise<visualization_msgs::Marker>("/MINCO_wpts", 0);
    trajPub = 
        nh.advertise<trajectory_msgs::JointTrajectory>("/reference_trajectory", 0);
    trajPubNoReset = 
        nh.advertise<trajectory_msgs::JointTrajectory>("/reference_trajectory_no_reset", 0);
    
    meshPub = 
        nh.advertise<visualization_msgs::Marker>("/visualizer/mesh", 1000);
    edgePub = 
        nh.advertise<visualization_msgs::Marker>("/visualizer/edge", 1000);

    goalPub = 
        nh.advertise<geometry_msgs::PoseStamped>("/global_planner/goal", 0);

    paddedLaserPub =
        nh.advertise<visualization_msgs::Marker>("/paddedObs", 0);

    jpsPub = 
        nh.advertise<nav_msgs::Path>("/jpsPath", 0);

    laserSub = nh.subscribe("/front/scan", 1, &Planner::lasercb, this);
    odomSub = nh.subscribe("/odometry/filtered", 1, &Planner::odomcb, this);
    pathSub = nh.subscribe("/global_planner/planner/plan", 1, &Planner::globalPathcb, this);
    goalSub = nh.subscribe("/planner_goal", 1, &Planner::goalcb, this);
    clickedPointSub = nh.subscribe("/clicked_point", 1, &Planner::clickedPointcb, this);

    controlTimer = nh.createTimer(ros::Duration(_dt), &Planner::controlLoop, this);
    goalTimer = nh.createTimer(ros::Duration(_dt/2.0), &Planner::goalLoop, this);

    _is_init = false;
    _is_goal_set = false;

    ROS_INFO("Initialized planner!");
}

void Planner::spin(){
    tf::TransformListener tfListener(ros::Duration(10));
    costmap = new costmap_2d::Costmap2DROS("costmap", tfListener);
    costmap->start();
    ros::AsyncSpinner spinner(1);
    spinner.start();
    ros::waitForShutdown();
}

void Planner::clickedPointcb(const geometry_msgs::PointStamped::ConstPtr& msg){
    goal = Eigen::VectorXd(2);
    goal(0) = msg->point.x;
    goal(1) = msg->point.y;
    _is_goal_set = true;
}

void Planner::goalcb(const geometry_msgs::PoseStamped::ConstPtr& msg){
    goal = Eigen::VectorXd(2);
    goal(0) = msg->pose.position.x;
    goal(1) = msg->pose.position.y;

    _is_goal_set = true;
}

void Planner::odomcb(const nav_msgs::Odometry::ConstPtr& msg){

    static ros::Time start;
    static Eigen::VectorXd _prevOdom;

	tf::Quaternion q(
	    msg->pose.pose.orientation.x,
	    msg->pose.pose.orientation.y,
	    msg->pose.pose.orientation.z,
	    msg->pose.pose.orientation.w
	);

	tf::Matrix3x3 m(q);
	double roll, pitch, yaw;
	m.getRPY(roll, pitch, yaw);

	_odom = Eigen::VectorXd(3);

	_odom(0) = msg->pose.pose.position.x;
	_odom(1) = msg->pose.pose.position.y; 
	_odom(2) = yaw;

    _vel = Eigen::VectorXd(3);
    _vel(0) = msg->twist.twist.linear.x*cos(yaw);
    _vel(1) = msg->twist.twist.linear.x*sin(yaw);
    _vel(2) = 0;

    _prevOdom = _odom;
    _is_init = true;
}

void Planner::lasercb(const sensor_msgs::LaserScan::ConstPtr& msg){
    
    if (!_is_init)
        return;

    _obs.clear();

	for (int i = 0; i < msg->ranges.size(); i++){
		if (msg->ranges[i] > msg->range_max || msg->ranges[i] < msg->range_min)
			continue;

        double d = msg->ranges[i];

		double angle = msg->angle_min + i*msg->angle_increment + _odom(2);
		double x = d*cos(angle) + _odom(0);
		double y = d*sin(angle) + _odom(1);

		_obs.push_back(Vec2f(x,y));
	}

    // vec_Vec2f padded = corridor::getPaddedScan(*costmap->getCostmap(), 0,0, _obs);
    
    // visualization_msgs::Marker pointMsg;
    // pointMsg.header.stamp = ros::Time::now();
    // pointMsg.header.frame_id = _frame_str;
    // pointMsg.type= visualization_msgs::Marker::POINTS;
    // pointMsg.action = visualization_msgs::Marker::ADD;
    // pointMsg.id = 123;
    // pointMsg.scale.x = .1;
    // pointMsg.color.r = 0;
    // pointMsg.color.g = 0;
    // pointMsg.color.b = 1;
    // pointMsg.color.a = 1;
    // for(Vec2f p : padded){
    //     geometry_msgs::Point pmsg;
    //     pmsg.x = p[0];
    //     pmsg.y = p[1];
    //     pmsg.z = 0;
    //     pointMsg.points.push_back(pmsg);
    // }

    // paddedLaserPub.publish(pointMsg);

}

void Planner::globalPathcb(const nav_msgs::Path::ConstPtr& msg){
    
    for (geometry_msgs::PoseStamped p : msg->poses)
        astarPath.push_back(Eigen::Vector2d(p.pose.position.x, p.pose.position.y));
}

template <int D>
void Planner::visualizeTraj(const Trajectory<D> &traj){

    if (traj.getPieceNum() > 0){
        nav_msgs::Path msg;
        msg.header.stamp = ros::Time::now();
        msg.header.frame_id = _frame_str;

        visualization_msgs::Marker wptMsg;
        wptMsg.header.stamp = ros::Time::now();
        wptMsg.header.frame_id = _frame_str;
        wptMsg.id = 0;
        wptMsg.type = visualization_msgs::Marker::SPHERE_LIST;
        wptMsg.ns = "waypoints";
        wptMsg.color.r = 1.00;
        wptMsg.color.g = 0.00;
        wptMsg.color.b = 0.00;
        wptMsg.color.a = 1.00;
        wptMsg.scale.x = 0.35;
        wptMsg.scale.y = 0.35;
        wptMsg.scale.z = 0.35;

        Eigen::MatrixXd wps = traj.getPositions();
        for (int i = 0; i < wps.cols(); i++){
            
            // geometry_msgs::Point point;
            // point.x = wps.col(i)(0);
            // point.y = wps.col(i)(1);
            // point.z = wps.col(i)(2);
            // wptMsg.points.push_back(point);

            geometry_msgs::PoseStamped pose;
            pose.header.stamp = ros::Time::now();
            pose.header.frame_id = _frame_str;
            pose.pose.position.x = wps.col(i)(0);
            pose.pose.position.y = wps.col(i)(1);
            pose.pose.position.z = wps.col(i)(2);

            pose.pose.orientation.x = 0;
            pose.pose.orientation.y = 0;
            pose.pose.orientation.z = 0;
            pose.pose.orientation.w = 1;
            msg.poses.push_back(pose);
        }

        for(int i = 0; i < traj.getPieceNum(); i++){
            Eigen::Vector3d pos = traj[i].getPos(0);
            geometry_msgs::Point point;
            point.x = pos(0);
            point.y = pos(1);
            point.z = pos(2);
            wptMsg.points.push_back(point);
        }


        wptVizPub.publish(wptMsg);
        trajVizPub.publish(msg);
    }

}

template <int D>
trajectory_msgs::JointTrajectory Planner::convertTrajToMsg(const Trajectory<D> &traj){

    trajectory_msgs::JointTrajectory msg;
    msg.header.stamp = ros::Time::now();
    msg.header.frame_id = _frame_str;

    double factor = traj.getMaxVelRate() / _max_vel;
    factor = 5;
    double t = 0.;

    std::vector<double> _times, _x, _y;
    while (t < traj.getTotalDuration()){
        Eigen::Vector3d pos = traj.getPos(t);

        _times.push_back(t*factor);
        _x.push_back(pos(0));
        _y.push_back(pos(1));

        t += .1/factor;

    }

    tk::spline sX = tk::spline(_times, _x, tk::spline::cspline,false,
                                tk::spline::first_deriv, _vel.x(),
                                tk::spline::first_deriv, _vel.x());
    tk::spline sY = tk::spline(_times, _y, tk::spline::cspline,false,
                                tk::spline::first_deriv, _vel.y(),
                                tk::spline::first_deriv, _vel.y());

    // while (t < traj.getTotalDuration()){
    //     Eigen::Vector3d pos = traj.getPos(t);
    //     Eigen::Vector3d vel = traj.getVel(t);
    //     Eigen::Vector3d acc = traj.getAcc(t);

    //     trajectory_msgs::JointTrajectoryPoint pt;
    //     pt.positions.push_back(pos(0));
    //     pt.positions.push_back(pos(1));

    //     pt.velocities.push_back(vel(0)/factor);
    //     pt.velocities.push_back(vel(1)/factor);

    //     pt.accelerations.push_back(acc(0)/(factor*factor));
    //     pt.accelerations.push_back(acc(1)/(factor*factor));

    //     pt.time_from_start = ros::Duration(t*factor);

    //     msg.points.push_back(pt);
        
    //     // ROS_INFO("%.2f/%.2f:\t(%.2f, %.2f)",t*factor,traj.getTotalDuration()*factor, pos(0), pos(1));

    //     t += .1/factor;
    // }

    t = 0;
    for(int i = 0; i < _times.size(); i++){
        trajectory_msgs::JointTrajectoryPoint pt;
        pt.positions.push_back(sX(t));
        pt.positions.push_back(sY(t));

        pt.velocities.push_back(sX.deriv(1,t));
        pt.velocities.push_back(sY.deriv(1,t));

        pt.accelerations.push_back(sX.deriv(2,t));
        pt.accelerations.push_back(sY.deriv(2,t));

        pt.time_from_start = ros::Duration(t);

        msg.points.push_back(pt);
        t += .1;
    }

    return msg;
}

void Planner::controlLoop(const ros::TimerEvent&){

    static ros::Time start;

    // if (!_is_init || astarPath.size() == 0 || !_is_goal_set)
    if (!_is_init || !_is_goal_set)
        return;

    ros::Time a = ros::Time::now();

    costmap_2d::Costmap2D* _map = costmap->getCostmap();
    costmap->resetLayers();
    costmap->updateMap();

    JPSPlanner jps;
    unsigned int sX, sY, eX, eY;
    _map->worldToMap(_odom(0), _odom(1), sX, sY);
    _map->worldToMap(goal(0), goal(1), eX, eY);

    jps.set_start(sX, sY);
    jps.set_destination(eX, eY);
    // traj.clear();

    jps.set_map(_map->getCharMap(), _map->getSizeInCellsX(), _map->getSizeInCellsY());
    jps.JPS();
    std::vector<Eigen::Vector2d> jpsPath = jps.getPath();

    if (jpsPath.size() == 0){
        ROS_INFO("JPS failed to find path");
        return;
    }


    for(int i = 0; i < jpsPath.size(); i++){
        double x, y;
        _map->mapToWorld(jpsPath[i](0), jpsPath[i](1), x, y);
        jpsPath[i] = Eigen::Vector2d(x,y);
    }

    nav_msgs::Path jpsMsg;
    jpsMsg.header.stamp = ros::Time::now();
    jpsMsg.header.frame_id = _frame_str;
    for(Eigen::Vector2d p : jpsPath){
        geometry_msgs::PoseStamped pMsg;
        pMsg.header = jpsMsg.header;
        pMsg.pose.position.x = p(0);
        pMsg.pose.position.y = p(1);
        pMsg.pose.position.z = 0;
        pMsg.pose.orientation.w = 1;
        jpsMsg.poses.push_back(pMsg);
    }

    jpsPub.publish(jpsMsg);

    // hPolys = corridor::createCorridor(astarPath, *_map, _obs);
    hPolys = corridor::createCorridorJPS(jpsPath, *_map, _obs);
    corridor::visualizePolytope(hPolys, meshPub, edgePub);

    // ROS_INFO("%lu/%lu", jpsPath.size(), hPolys.size());

    // initial and final states
    Eigen::Matrix3d initialPVA;
    bool not_first = false;
    if (traj.getPieceNum() == 0 || _is_teleop){
        initialPVA <<   Eigen::Vector3d(_odom(0),_odom(1),0), 
                        Eigen::Vector3d::Zero(), 
                        Eigen::Vector3d::Zero();

        // std::cout << initialPVA << std::endl;
    }
    else{
        not_first = true;
        double maxVel = traj.getMaxVelRate();
        double factor = maxVel / _max_vel;
        factor = 5;
        // double t = factor < 1 ? (a-start).toSec() + .1 : (a-start).toSec()/factor + .1;
        double t = (a-start).toSec()/factor + .1;
        double tmpT = t;
        
        int pieceIdx = traj.locatePieceIdx(tmpT);
        Piece<5> currPiece = traj[pieceIdx];
        double pieceDuration = currPiece.getDuration();

        // initialPVA.col(0) = traj.getPos(t);
        // initialPVA.col(1) = traj.getVel(t);
        // initialPVA.col(2) = traj.getAcc(t);

        // double pieceStartT = 0;
        // for (int i = 0; i < pieceIdx; i++){
        //     pieceStartT += traj[i].getDuration();
        // }

        // initialPVA.col(0) = currPiece.getPos(pieceStartT + pieceDuration);
        initialPVA.col(0) = currPiece.getPos(pieceDuration);
        
        // if (factor > 1){
        //     initialPVA.col(1) = currPiece.getVel(pieceStartT + pieceDuration)/factor;
        //     initialPVA.col(2) = currPiece.getAcc(pieceStartT + pieceDuration)/(factor*factor);
        // } else{
        //     initialPVA.col(1) = currPiece.getVel(pieceStartT + pieceDuration);
        //     initialPVA.col(2) = currPiece.getAcc(pieceStartT + pieceDuration);
        // }

        initialPVA.col(1) = currPiece.getVel(pieceDuration);
        initialPVA.col(2) = currPiece.getAcc(pieceDuration);

        // std::cout << "piece: " << traj.locatePieceIdx(t) << "/" << traj.getPieceNum() << std::endl;
        // std::cout << t << "/" << traj.getTotalDuration() << std::endl;
        std::cout << t << "\t" << factor << "\t";
        std::cout << pieceIdx << "/" << traj.getPieceNum() <<std::endl;
        std::cout << (a-start).toSec() << "\t" << (a-start).toSec()/factor << std::endl;
        std::cout << initialPVA.col(0) << std::endl;
        std::cout << "*******************************" << std::endl;

    }


    Eigen::Matrix3d finalPVA;
    finalPVA << Eigen::Vector3d(goal(0),goal(1),0), 
                Eigen::Vector3d::Zero(), 
                Eigen::Vector3d::Zero();

    Eigen::VectorXd magnitudeBounds(5);
    Eigen::VectorXd penaltyWeights(5);
    Eigen::VectorXd physicalParams(6);
    magnitudeBounds(0) = 0.5;   //v_max
    magnitudeBounds(1) = .8;   //omg_max
    magnitudeBounds(2) = .3;    //theta_max
    magnitudeBounds(3) = -1;     //thrust_min
    magnitudeBounds(4) = .1;    //thrust_max
    penaltyWeights(0) = 1e4;    //pos_weight
    penaltyWeights(1) = 1e4;    //vel_weight
    penaltyWeights(2) = 1e4;    //omg_weight
    penaltyWeights(3) = 1e4;    //theta_weight
    penaltyWeights(4) = 1e5;    //thrust_weight
    physicalParams(0) = .61;    // mass
    physicalParams(1) = 9.81;   // gravity
    physicalParams(2) = 0;      // drag
    physicalParams(3) = 0;      // drag
    physicalParams(4) = 0;      // drag
    physicalParams(5) = .0001;  // speed smooth factor

    gcopter::GCOPTER_PolytopeSFC gcopter;

    if(!gcopter.setup(
        20.0,   //time weight
        initialPVA, 
        finalPVA,
        hPolys,
        1e6,    // lengthPerPiece
        1e-2,   // smoothing factor
        16,     // integral resolution
        magnitudeBounds,
        penaltyWeights,
        physicalParams
    )){
        ROS_INFO("optimizer setup failed");
        return;
    }

    Trajectory<5> newTraj;
    if (std::isinf(gcopter.optimize(newTraj, 1e-5))){
        ROS_INFO("solver could not find trajectory");
        return;
    }

    // if (std::isinf(gcopter.optimize(traj, 1e-5))){
    //     ROS_INFO("solver could not find trajectory");
    //     return;
    // }

    if (traj.getPieceNum() != 0){
        double maxVel = traj.getMaxVelRate();
        double factor = maxVel / _max_vel;
        factor = 5;
        // double t = factor < 1 ? (a-start).toSec() + .1 : (a-start).toSec()/factor + .1;
        double t = (a-start).toSec()/factor + .1;

        int pieceIdx = traj.locatePieceIdx(t);

        std::vector<double> durs;
        std::vector<typename Piece<5>::CoefficientMat> cMats;
        for(int i = 0; i <= pieceIdx; i++){
            durs.push_back(traj[i].getDuration());
            cMats.push_back(traj[i].getCoeffMat());
        }

        Trajectory<5> tmpTraj(durs, cMats);
        tmpTraj.append(newTraj);
        traj = tmpTraj;
        if (!_is_teleop)
            trajPubNoReset.publish(convertTrajToMsg(traj));
    }else{
        ROS_INFO("trajectory has been overwritten");
        traj = newTraj;
        if (!_is_teleop)
            trajPub.publish(convertTrajToMsg(traj));
        start = ros::Time::now();
    }

    // if (!_is_teleop)
    //     trajPub.publish(convertTrajToMsg(traj));

    visualizeTraj(traj);
    // exit(0);

    double totalT = (ros::Time::now() - a).toSec();
    std::cout << "total time is " << totalT << std::endl;
    astarPath.clear();
    _is_goal_set = false;


    // if (not_first)
    //     exit(0);
}

void Planner::goalLoop(const ros::TimerEvent&){

    if (!_is_goal_set)
        return;

    geometry_msgs::PoseStamped msg;
    msg.header.stamp = ros::Time::now();
    msg.header.frame_id = _frame_str;

    // msg.pose.position.x = 5.2;
    // msg.pose.position.y = -.3;
    msg.pose.position.x = goal(0);
    msg.pose.position.y = goal(1);
    msg.pose.position.z = 0;
    msg.pose.orientation.w = 1;

    goalPub.publish(msg);
}
