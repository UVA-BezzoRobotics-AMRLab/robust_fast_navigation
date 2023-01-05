#ifndef CORRIDOR_H
#define CORRIDOR_H

#include <ros/ros.h>
#include <visualization_msgs/Marker.h>

#include <decomp_util/seed_decomp.h>
#include <decomp_util/line_segment.h>
#include <costmap_2d/costmap_2d_ros.h>
#include <decomp_geometry/geometric_utils.h>

namespace corridor{

    // following bresenham / raycast method taken with <3 from https://docs.ros.org/en/api/costmap_2d/html/costmap__2d_8h_source.html
    inline void bresenham(const costmap_2d::Costmap2D& _map, unsigned int abs_da, unsigned int abs_db, int error_b, int offset_a,
        int offset_b, unsigned int offset, unsigned int max_range, unsigned int& term){
        
        unsigned int end = std::min(max_range, abs_da);
        unsigned int mx, my;
        for (unsigned int i = 0; i < end; ++i) {
            offset += offset_a;
            error_b += abs_db;
            
            _map.indexToCells(offset, mx, my);
            if (_map.getCost(mx, my) != costmap_2d::FREE_SPACE){
                // ROS_INFO("oh shoot bresenham found something!");
                break;
            }

            if ((unsigned int)error_b >= abs_da) {
                offset += offset_b;
                error_b -= abs_da;
            }

            _map.indexToCells(offset, mx, my);
            if (_map.getCost(mx, my) != costmap_2d::FREE_SPACE){
                // ROS_INFO("oh shoot bresenham found something!");
                break;
            }
        }
        
        term = offset;
    }


    inline void raycast(const costmap_2d::Costmap2D& _map, unsigned int sx, unsigned int sy, 
        unsigned int ex, unsigned int ey, double &x, double &y,
        unsigned int max_range = 1e6){
        
        unsigned int size_x = _map.getSizeInCellsX();

        int dx = ex - sx;
        int dy = ey - sy;

        unsigned int abs_dx = abs(dx);
        unsigned int abs_dy = abs(dy);
        
        int offset_dx = dx > 0 ? 1 : -1;
        int offset_dy = (dy > 0 ? 1 : -1) * _map.getSizeInCellsX();

        unsigned int offset = sy * size_x + sx;

        double dist = hypot(dx, dy);
        double scale = (dist == 0.0) ? 1.0 : std::min(1.0, max_range / dist);

        unsigned int term; 
        if (abs_dx >= abs_dy){
            int error_y = abs_dx / 2;
            bresenham(_map, abs_dx, abs_dy, error_y, offset_dx, offset_dy, 
                    offset, (unsigned int)(scale * abs_dx), term);
        } else{
            int error_x = abs_dy / 2;
            bresenham(_map, abs_dy, abs_dx, error_x, offset_dy, offset_dx,
                    offset, (unsigned int)(scale * abs_dy), term);
        }

        // convert costmap index to world coordinates
        unsigned int mx, my;
        _map.indexToCells(term, mx, my);
        _map.mapToWorld(mx, my, x, y);
    }

    inline Eigen::MatrixX4d getHyperPlanes(const Polyhedron<2>& poly, const Eigen::Vector2d& seed){
        
        vec_E<Hyperplane<2> > planes = poly.vs_;

        auto vertices = cal_vertices(poly);
        vertices.push_back(vertices[0]);

        Eigen::MatrixX4d hPoly(vertices.size()+1, 4);

        for(int i = 0; i < vertices.size()-1; i++){

            // hyperplane from vertex to vertex
            Vecf<2> v1 = vertices[i];
            Vecf<2> v2 = vertices[i+1];
            Eigen::Vector4d hypEq;
            if (fabs(v1(0)-v2(0)) < 1e-6)
                hypEq << 1,0,0,-v1(0);
            else{
                double m = (v2(1)-v1(1))/(v2(0)-v1(0));
                hypEq << -m,1,0,-v1(1)+m*v1(0);
            }
            
            if (hypEq(0)*seed(0)+hypEq(1)*seed(1)+hypEq(2)*.5+hypEq(3) > 0)
                hypEq *= -1;

            hPoly.row(i) = hypEq;
        }

        Eigen::Vector4d hypEqZ0(0,0,1,-.5);
        if (hypEqZ0(0)*seed(0)+hypEqZ0(1)*seed(1)+hypEqZ0(2)*.5+ hypEqZ0(3) > 0)
            hypEqZ0 *= -1;

        Eigen::Vector4d hypEqZ1(0,0,-1,-.5);
        if (hypEqZ1(0)*seed(0)+hypEqZ1(1)*seed(1)+hypEqZ1(2)*.5+ hypEqZ1(3) > 0)
            hypEqZ1 *= -1;

        hPoly.row(vertices.size()-1) = hypEqZ0;
        hPoly.row(vertices.size()) = hypEqZ1;

        return hPoly;
    }

    inline vec_Vec2f getPaddedScan(const costmap_2d::Costmap2D& _map, 
                                    double seedX, double seedY, const vec_Vec2f& _obs){

        int offset = 1000000;

        double x, y;
        unsigned int sx, sy, ex, ey;

        vec_Vec2f paddedObs;

        if (!_map.worldToMap(seedX, seedY, sx, sy)){
            ROS_ERROR("FATAL: Odometry reading is NOT inside costmap!");
            // exit(-1);
            return paddedObs;
        }

        for(Vec2f ob : _obs){
            if (!_map.worldToMap(ob[0], ob[1], ex, ey))
                continue;

            corridor::raycast(_map, sx,sy,ex,ey,x,y);
            paddedObs.push_back(Vec2f(x,y));
         
        }

        return paddedObs;
    }


    inline Eigen::MatrixX4d genPoly(const costmap_2d::Costmap2D& _map, 
                                    double x, double y, const vec_Vec2f& _obs){
        SeedDecomp2D decomp(Vec2f(x, y));
        // decomp.set_obs(_obs);
        decomp.set_obs(getPaddedScan(_map, x, y, _obs));
        decomp.set_local_bbox(Vec2f(2,2));
        decomp.dilate(.1);

        auto poly = decomp.get_polyhedron();
    
        return getHyperPlanes(poly, Eigen::Vector2d(x,y));
    }

    inline Eigen::MatrixX4d genPolyJPS(const costmap_2d::Costmap2D& _map, 
                                    const Eigen::Vector2d& p1, const Eigen::Vector2d& p2,
                                    const vec_Vec2f& _obs){
        LineSegment2D decomp(p1, p2);
        // decomp.set_obs(_obs);
        double x = ((p1+p2)/2)(0);
        double y = ((p1+p2)/2)(1);
        decomp.set_obs(getPaddedScan(_map, x, y, _obs));
        decomp.set_local_bbox(Vec2f(2,2));
        decomp.dilate(.1);

        auto poly = decomp.get_polyhedron();
    
        return getHyperPlanes(poly, Eigen::Vector2d(x,y));
    }

    inline void visualizePolytope(const std::vector<Eigen::MatrixX4d> &hPolys,
                                    const ros::Publisher& meshPub,
                                    const ros::Publisher& edgePub){

        // Due to the fact that H-representation cannot be directly visualized
        // We first conduct vertex enumeration of them, then apply quickhull
        // to obtain triangle meshs of polyhedra
        Eigen::Matrix3Xd mesh(3, 0), curTris(3, 0), oldTris(3, 0);
        for (size_t id = 0; id < hPolys.size(); id++)
        {
            oldTris = mesh;
            Eigen::Matrix<double, 3, -1, Eigen::ColMajor> vPoly;
            geo_utils::enumerateVs(hPolys[id], vPoly);

            quickhull::QuickHull<double> tinyQH;
            const auto polyHull = tinyQH.getConvexHull(vPoly.data(), vPoly.cols(), false, true);
            const auto &idxBuffer = polyHull.getIndexBuffer();
            int hNum = idxBuffer.size() / 3;

            curTris.resize(3, hNum * 3);
            for (int i = 0; i < hNum * 3; i++)
            {
                curTris.col(i) = vPoly.col(idxBuffer[i]);
            }
            mesh.resize(3, oldTris.cols() + curTris.cols());
            mesh.leftCols(oldTris.cols()) = oldTris;
            mesh.rightCols(curTris.cols()) = curTris;
        }

        // RVIZ support tris for visualization
        visualization_msgs::Marker meshMarker, edgeMarker;

        meshMarker.id = 0;
        meshMarker.header.stamp = ros::Time::now();
        meshMarker.header.frame_id = "map";
        meshMarker.pose.orientation.w = 1.00;
        meshMarker.action = visualization_msgs::Marker::ADD;
        meshMarker.type = visualization_msgs::Marker::TRIANGLE_LIST;
        meshMarker.ns = "mesh";
        meshMarker.color.r = 0.00;
        meshMarker.color.g = 0.00;
        meshMarker.color.b = 1.00;
        meshMarker.color.a = 0.15;
        meshMarker.scale.x = 1.0;
        meshMarker.scale.y = 1.0;
        meshMarker.scale.z = 1.0;

        edgeMarker = meshMarker;
        edgeMarker.type = visualization_msgs::Marker::LINE_LIST;
        edgeMarker.ns = "edge";
        edgeMarker.color.r = 0.00;
        edgeMarker.color.g = 1.00;
        edgeMarker.color.b = 1.00;
        edgeMarker.color.a = 1.00;
        edgeMarker.scale.x = 0.02;

        geometry_msgs::Point point;

        int ptnum = mesh.cols();

        for (int i = 0; i < ptnum; i++)
        {
            point.x = mesh(0, i);
            point.y = mesh(1, i);
            point.z = mesh(2, i);
            meshMarker.points.push_back(point);
        }

        for (int i = 0; i < ptnum / 3; i++)
        {
            for (int j = 0; j < 3; j++)
            {
                point.x = mesh(0, 3 * i + j);
                point.y = mesh(1, 3 * i + j);
                point.z = mesh(2, 3 * i + j);
                edgeMarker.points.push_back(point);
                point.x = mesh(0, 3 * i + (j + 1) % 3);
                point.y = mesh(1, 3 * i + (j + 1) % 3);
                point.z = mesh(2, 3 * i + (j + 1) % 3);
                edgeMarker.points.push_back(point);
            }
        }

        meshPub.publish(meshMarker);
        edgePub.publish(edgeMarker);

        return;
    }


    inline bool isInPoly(const Eigen::MatrixX4d& poly, const Eigen::Vector2d& p){
        // point needs to be p=(x,y,z,1)
        // in 2D so z = 0
        Eigen::Vector4d pR4(p(0), p(1), 0, 1);
        Eigen::VectorXd res = poly*pR4;

        for (int i = 0; i < res.rows(); i++){
            if (res(i) > 0)
                return false;
        }
        return true;

    }


    inline std::vector<Eigen::MatrixX4d> createCorridor(
        const std::vector<Eigen::Vector2d>& path, const costmap_2d::Costmap2D& _map,
        const vec_Vec2f& _obs){
        
        int max_iters = 1000;

        std::vector<Eigen::MatrixX4d> polys;
        Eigen::Vector2d seed(path[0](0), path[0](1));
        polys.push_back(genPoly(_map, seed(0), seed(1), _obs));

        Eigen::Vector2d goal = path.back();

        int i = 0;
        int lastStopped = 0;
        while (i < max_iters && !isInPoly(polys.back(), goal)){

            Eigen::Vector2d newSeed;
            for (int k = lastStopped; k < path.size(); k++){
                if (!isInPoly(polys.back(), path[k]) || k == path.size()-1){
                    lastStopped = k;
                    newSeed = path[k];
                    break;
                }
            }

            polys.push_back(genPoly(_map, newSeed(0), newSeed(1), _obs));
            i++;
        }

        return polys;
    }

    inline std::vector<Eigen::MatrixX4d> createCorridorJPS(
        const std::vector<Eigen::Vector2d>& path, const costmap_2d::Costmap2D& _map,
        const vec_Vec2f& _obs){

        std::vector<Eigen::MatrixX4d> polys;
        for(int i = 0; i < path.size()-1; i++){
            polys.push_back(genPolyJPS(_map, path[i], path[i+1], _obs));
        }
        
        return polys;
    }


}

#endif