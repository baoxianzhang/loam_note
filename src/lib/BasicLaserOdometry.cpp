#include "loam_velodyne/BasicLaserOdometry.h"

#include "math_utils.h"
#include <pcl/filters/filter.h>
#include <Eigen/Eigenvalues>
#include <Eigen/QR>

namespace loam
{

using std::sin;
using std::cos;
using std::asin;
using std::atan2;
using std::sqrt;
using std::fabs;
using std::pow;


BasicLaserOdometry::BasicLaserOdometry(float scanPeriod, size_t maxIterations) :
   _scanPeriod(scanPeriod),
   _systemInited(false),
   _frameCount(0),
   _maxIterations(maxIterations),
   _deltaTAbort(0.1),
   _deltaRAbort(0.1),
   _cornerPointsSharp(new pcl::PointCloud<pcl::PointXYZI>()),
   _cornerPointsLessSharp(new pcl::PointCloud<pcl::PointXYZI>()),
   _surfPointsFlat(new pcl::PointCloud<pcl::PointXYZI>()),
   _surfPointsLessFlat(new pcl::PointCloud<pcl::PointXYZI>()),
   _laserCloud(new pcl::PointCloud<pcl::PointXYZI>()),
   _lastCornerCloud(new pcl::PointCloud<pcl::PointXYZI>()),
   _lastSurfaceCloud(new pcl::PointCloud<pcl::PointXYZI>()),
   _laserCloudOri(new pcl::PointCloud<pcl::PointXYZI>()),
   _coeffSel(new pcl::PointCloud<pcl::PointXYZI>())
{}


//当前点云中的点相对第一个点去除因匀速运动产生的畸变，效果相当于得到在点云扫描开始位置静止扫描得到的点云
void BasicLaserOdometry::transformToStart(const pcl::PointXYZI& pi, pcl::PointXYZI& po)
{
   //插值系数计算
   float s = (1.f / _scanPeriod) * (pi.intensity - int(pi.intensity));

   po.x = pi.x - s * _transform.pos.x();
   po.y = pi.y - s * _transform.pos.y();
   po.z = pi.z - s * _transform.pos.z();
   po.intensity = pi.intensity;

   Angle rx = -s * _transform.rot_x.rad();
   Angle ry = -s * _transform.rot_y.rad();
   Angle rz = -s * _transform.rot_z.rad();
   rotateZXY(po, rz, rx, ry);
}


//将上一帧点云中的点相对结束位置去除因匀速运动产生的畸变，效果相当于得到在点云扫描结束位置静止扫描得到的点云
size_t BasicLaserOdometry::transformToEnd(pcl::PointCloud<pcl::PointXYZI>::Ptr& cloud)
{
   size_t cloudSize = cloud->points.size();

   for (size_t i = 0; i < cloudSize; i++)
   {
      pcl::PointXYZI& point = cloud->points[i];

      float s = (1.f / _scanPeriod) * (point.intensity - int(point.intensity));

      point.x -= s * _transform.pos.x();
      point.y -= s * _transform.pos.y();
      point.z -= s * _transform.pos.z();
      point.intensity = int(point.intensity);

      Angle rx = -s * _transform.rot_x.rad();
      Angle ry = -s * _transform.rot_y.rad();
      Angle rz = -s * _transform.rot_z.rad();
      rotateZXY(point, rz, rx, ry);
      rotateYXZ(point, _transform.rot_y, _transform.rot_x, _transform.rot_z);

      point.x += _transform.pos.x() - _imuShiftFromStart.x();
      point.y += _transform.pos.y() - _imuShiftFromStart.y();
      point.z += _transform.pos.z() - _imuShiftFromStart.z();

      rotateZXY(point, _imuRollStart, _imuPitchStart, _imuYawStart);
      rotateYXZ(point, -_imuYawEnd, -_imuPitchEnd, -_imuRollEnd);
   }

   return cloudSize;
}


//利用IMU修正旋转量，根据起始欧拉角，当前点云的欧拉角修正
void BasicLaserOdometry::pluginIMURotation(const Angle& bcx, const Angle& bcy, const Angle& bcz,
                                           const Angle& blx, const Angle& bly, const Angle& blz,
                                           const Angle& alx, const Angle& aly, const Angle& alz,
                                           Angle &acx, Angle &acy, Angle &acz)
{
   float sbcx = bcx.sin();
   float cbcx = bcx.cos();
   float sbcy = bcy.sin();
   float cbcy = bcy.cos();
   float sbcz = bcz.sin();
   float cbcz = bcz.cos();

   float sblx = blx.sin();
   float cblx = blx.cos();
   float sbly = bly.sin();
   float cbly = bly.cos();
   float sblz = blz.sin();
   float cblz = blz.cos();

   float salx = alx.sin();
   float calx = alx.cos();
   float saly = aly.sin();
   float caly = aly.cos();
   float salz = alz.sin();
   float calz = alz.cos();

   float srx = -sbcx * (salx*sblx + calx * caly*cblx*cbly + calx * cblx*saly*sbly)
      - cbcx * cbcz*(calx*saly*(cbly*sblz - cblz * sblx*sbly)
                     - calx * caly*(sbly*sblz + cbly * cblz*sblx) + cblx * cblz*salx)
      - cbcx * sbcz*(calx*caly*(cblz*sbly - cbly * sblx*sblz)
                     - calx * saly*(cbly*cblz + sblx * sbly*sblz) + cblx * salx*sblz);
   acx = -asin(srx);

   float srycrx = (cbcy*sbcz - cbcz * sbcx*sbcy)*(calx*saly*(cbly*sblz - cblz * sblx*sbly)
                                                  - calx * caly*(sbly*sblz + cbly * cblz*sblx) + cblx * cblz*salx)
      - (cbcy*cbcz + sbcx * sbcy*sbcz)*(calx*caly*(cblz*sbly - cbly * sblx*sblz)
                                        - calx * saly*(cbly*cblz + sblx * sbly*sblz) + cblx * salx*sblz)
      + cbcx * sbcy*(salx*sblx + calx * caly*cblx*cbly + calx * cblx*saly*sbly);
   float crycrx = (cbcz*sbcy - cbcy * sbcx*sbcz)*(calx*caly*(cblz*sbly - cbly * sblx*sblz)
                                                  - calx * saly*(cbly*cblz + sblx * sbly*sblz) + cblx * salx*sblz)
      - (sbcy*sbcz + cbcy * cbcz*sbcx)*(calx*saly*(cbly*sblz - cblz * sblx*sbly)
                                        - calx * caly*(sbly*sblz + cbly * cblz*sblx) + cblx * cblz*salx)
      + cbcx * cbcy*(salx*sblx + calx * caly*cblx*cbly + calx * cblx*saly*sbly);
   acy = atan2(srycrx / acx.cos(), crycrx / acx.cos());

   float srzcrx = sbcx * (cblx*cbly*(calz*saly - caly * salx*salz) - cblx * sbly*(caly*calz + salx * saly*salz) + calx * salz*sblx)
      - cbcx * cbcz*((caly*calz + salx * saly*salz)*(cbly*sblz - cblz * sblx*sbly)
                     + (calz*saly - caly * salx*salz)*(sbly*sblz + cbly * cblz*sblx)
                     - calx * cblx*cblz*salz)
      + cbcx * sbcz*((caly*calz + salx * saly*salz)*(cbly*cblz + sblx * sbly*sblz)
                     + (calz*saly - caly * salx*salz)*(cblz*sbly - cbly * sblx*sblz)
                     + calx * cblx*salz*sblz);
   float crzcrx = sbcx * (cblx*sbly*(caly*salz - calz * salx*saly) - cblx * cbly*(saly*salz + caly * calz*salx) + calx * calz*sblx)
      + cbcx * cbcz*((saly*salz + caly * calz*salx)*(sbly*sblz + cbly * cblz*sblx)
                     + (caly*salz - calz * salx*saly)*(cbly*sblz - cblz * sblx*sbly)
                     + calx * calz*cblx*cblz)
      - cbcx * sbcz*((saly*salz + caly * calz*salx)*(cblz*sbly - cbly * sblx*sblz)
                     + (caly*salz - calz * salx*saly)*(cbly*cblz + sblx * sbly*sblz)
                     - calx * calz*cblx*sblz);
   acz = atan2(srzcrx / acx.cos(), crzcrx / acx.cos());
}


//相对于第一个点云即原点，积累旋转量
void BasicLaserOdometry::accumulateRotation(Angle cx, Angle cy, Angle cz,
                                            Angle lx, Angle ly, Angle lz,
                                            Angle &ox, Angle &oy, Angle &oz)
{
   float srx = lx.cos()*cx.cos()*ly.sin()*cz.sin()
      - cx.cos()*cz.cos()*lx.sin()
      - lx.cos()*ly.cos()*cx.sin();
   ox = -asin(srx);

   float srycrx = lx.sin()*(cy.cos()*cz.sin() - cz.cos()*cx.sin()*cy.sin())
      + lx.cos()*ly.sin()*(cy.cos()*cz.cos() + cx.sin()*cy.sin()*cz.sin())
      + lx.cos()*ly.cos()*cx.cos()*cy.sin();
   float crycrx = lx.cos()*ly.cos()*cx.cos()*cy.cos()
      - lx.cos()*ly.sin()*(cz.cos()*cy.sin() - cy.cos()*cx.sin()*cz.sin())
      - lx.sin()*(cy.sin()*cz.sin() + cy.cos()*cz.cos()*cx.sin());
   oy = atan2(srycrx / ox.cos(), crycrx / ox.cos());

   float srzcrx = cx.sin()*(lz.cos()*ly.sin() - ly.cos()*lx.sin()*lz.sin())
      + cx.cos()*cz.sin()*(ly.cos()*lz.cos() + lx.sin()*ly.sin()*lz.sin())
      + lx.cos()*cx.cos()*cz.cos()*lz.sin();
   float crzcrx = lx.cos()*lz.cos()*cx.cos()*cz.cos()
      - cx.cos()*cz.sin()*(ly.cos()*lz.sin() - lz.cos()*lx.sin()*ly.sin())
      - cx.sin()*(ly.sin()*lz.sin() + ly.cos()*lz.cos()*lx.sin());
   oz = atan2(srzcrx / ox.cos(), crzcrx / ox.cos());
}

//被LaserOdometry::imuTransHandler调用，接收带有IMU数据的topic并提取IMU信息
void BasicLaserOdometry::updateIMU(pcl::PointCloud<pcl::PointXYZ> const& imuTrans)
{
   assert(4 == imuTrans.size());
   _imuPitchStart = imuTrans.points[0].x;
   _imuYawStart = imuTrans.points[0].y;
   _imuRollStart = imuTrans.points[0].z;

   _imuPitchEnd = imuTrans.points[1].x;
   _imuYawEnd = imuTrans.points[1].y;
   _imuRollEnd = imuTrans.points[1].z;

   _imuShiftFromStart = imuTrans.points[2];
   _imuVeloFromStart = imuTrans.points[3];
}

void BasicLaserOdometry::process()
{
   if (!_systemInited)
   {//运动估计需要前后两帧点云，刚收到第一帧点云时先不进行处理，得到收到第二帧再进行处理
      //并保证上一次的点云_lastCornerCloud存储的是上一帧点云中曲率较大的点云，即带有less的点云
      _cornerPointsLessSharp.swap(_lastCornerCloud);
      _surfPointsLessFlat.swap(_lastSurfaceCloud);

      //使用上一帧的点云特征点构建kd-tree，方便查找最近点
      _lastCornerKDTree.setInputCloud(_lastCornerCloud);
      _lastSurfaceKDTree.setInputCloud(_lastSurfaceCloud);

      _transformSum.rot_x += _imuPitchStart;
      _transformSum.rot_z += _imuRollStart;

      _systemInited = true;
      return;
   }

   pcl::PointXYZI coeff;
   bool isDegenerate = false;//退化标志
   Eigen::Matrix<float, 6, 6> matP;//P矩阵，预测矩阵

   _frameCount++;
   //T平移量的初值赋值为加减速的位移量，为其梯度下降的方向
   //（沿用上次转换的T（一个sweep匀速模型），同时在其基础上减去匀速运动位移，即只考虑加减速的位移量）
   _transform.pos -= _imuVeloFromStart * _scanPeriod;


   size_t lastCornerCloudSize = _lastCornerCloud->points.size();
   size_t lastSurfaceCloudSize = _lastSurfaceCloud->points.size();

   if (lastCornerCloudSize > 10 && lastSurfaceCloudSize > 100)
   {
      std::vector<int> pointSearchInd(1);
      std::vector<float> pointSearchSqDis(1);
      std::vector<int> indices;

      pcl::removeNaNFromPointCloud(*_cornerPointsSharp, *_cornerPointsSharp, indices);
      size_t cornerPointsSharpNum = _cornerPointsSharp->points.size();
      size_t surfPointsFlatNum = _surfPointsFlat->points.size();

      _pointSearchCornerInd1.resize(cornerPointsSharpNum);
      _pointSearchCornerInd2.resize(cornerPointsSharpNum);
      _pointSearchSurfInd1.resize(surfPointsFlatNum);
      _pointSearchSurfInd2.resize(surfPointsFlatNum);
      _pointSearchSurfInd3.resize(surfPointsFlatNum);

      //Levenberg-Marquardt算法(L-M method)，非线性最小二乘算法，最优化算法的一种
      //最多迭代25次
      for (size_t iterCount = 0; iterCount < _maxIterations; iterCount++)
      {
         pcl::PointXYZI pointSel, pointProj, tripod1, tripod2, tripod3;
         _laserCloudOri->clear();
         _coeffSel->clear();

         //处理edge point，
         //处理当前点云中的曲率最大的特征点,从上个点云中曲率比较大的特征点中找两个最近距离点，
         //一个点使用kd-tree查找，另一个根据找到的点在其相邻线找另外一个最近距离的点
         for (int i = 0; i < cornerPointsSharpNum; i++)
         {
            transformToStart(_cornerPointsSharp->points[i], pointSel);

            if (iterCount % 5 == 0)
            {//每迭代五次，重新查找最近点
               //kd-tree查找一个最近距离点，边沿点未经过体素栅格滤波，一般边沿点本来就比较少，不做滤波
               //pointSearchInd——最近点的序号，pointSearchSqDis——离最近点的距离
               pcl::removeNaNFromPointCloud(*_lastCornerCloud, *_lastCornerCloud, indices);
               _lastCornerKDTree.nearestKSearch(pointSel, 1, pointSearchInd, pointSearchSqDis);
               int closestPointInd = -1, minPointInd2 = -1;

              //寻找相邻线距离目标点距离最小的点
              //再次提醒：velodyne是2度一线，scanID相邻并不代表线号相邻，相邻线度数相差2度，也即线号scanID相差2
               if (pointSearchSqDis[0] < 25)//找到的最近点距离的确很近的话
               {
                  //提取最近点线号
                  closestPointInd = pointSearchInd[0];
                  int closestPointScan = int(_lastCornerCloud->points[closestPointInd].intensity);

                  float pointSqDis, minPointSqDis2 = 25;//初始门槛值5米，可大致过滤掉scanID相邻，但实际线不相邻的值
                  for (int j = closestPointInd + 1; j < cornerPointsSharpNum; j++)
                  {//向scanID增大的方向查找，实际是按照点云中点的序号朝增大的方向查找，同时不能超过点云中点的数量
                     if (int(_lastCornerCloud->points[j].intensity) > closestPointScan + 2.5)
                     {//非相邻线
                        break;
                     }

                     pointSqDis = calcSquaredDiff(_lastCornerCloud->points[j], pointSel);

                     if (int(_lastCornerCloud->points[j].intensity) > closestPointScan)
                     {//确保两个点不在同一条scan上（相邻线查找应该可以用scanID == closestPointScan +/- 1 来做）
                        if (pointSqDis < minPointSqDis2)
                        {//距离更近，要小于初始值5米
                           //更新最小距离与点序
                           minPointSqDis2 = pointSqDis;
                           minPointInd2 = j;
                        }
                     }
                  }
                  for (int j = closestPointInd - 1; j >= 0; j--)
                  {//向scanID减小的方向查找
                     if (int(_lastCornerCloud->points[j].intensity) < closestPointScan - 2.5)
                     {
                        break;
                     }

                     pointSqDis = calcSquaredDiff(_lastCornerCloud->points[j], pointSel);

                     if (int(_lastCornerCloud->points[j].intensity) < closestPointScan)
                     {
                        if (pointSqDis < minPointSqDis2)
                        {
                           minPointSqDis2 = pointSqDis;
                           minPointInd2 = j;
                        }
                     }
                  }
               }
               //记住组成线的点序
               _pointSearchCornerInd1[i] = closestPointInd;//kd-tree最近距离点，-1表示未找到满足的点
               _pointSearchCornerInd2[i] = minPointInd2;//另一个最近的，-1表示未找到满足的点
            }

            //计算edge point到上一帧点云中与之最近的edge line的距离
            //参考论文"ow-drift and real-time lidar odometry and mapping"
            if (_pointSearchCornerInd2[i] >= 0)
            {//大于等于0，不等于-1，说明两个点都找到了
               tripod1 = _lastCornerCloud->points[_pointSearchCornerInd1[i]];
               tripod2 = _lastCornerCloud->points[_pointSearchCornerInd2[i]];

               //选择的特征点记为O，kd-tree最近距离点记为A，另一个最近距离点记为B
               float x0 = pointSel.x;//O
               float y0 = pointSel.y;
               float z0 = pointSel.z;
               float x1 = tripod1.x;//A
               float y1 = tripod1.y;
               float z1 = tripod1.z;
               float x2 = tripod2.x;//B
               float y2 = tripod2.y;
               float z2 = tripod2.z;

               //公式(2)
               //向量OA = (x0 - x1, y0 - y1, z0 - z1), 向量OB = (x0 - x2, y0 - y2, z0 - z2)，向量AB = （x1 - x2, y1 - y2, z1 - z2）
               //向量OA OB的向量积(即叉乘)为：
               //|  i      j      k  |
               //|x0-x1  y0-y1  z0-z1|
               //|x0-x2  y0-y2  z0-z2|
               //模为：
               float a012 = sqrt(((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                                 * ((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                                 + ((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))
                                 * ((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))
                                 + ((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1))
                                 * ((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1)));

               //两个最近距离点之间的距离，即向量AB的模
               float l12 = sqrt((x1 - x2)*(x1 - x2) + (y1 - y2)*(y1 - y2) + (z1 - z2)*(z1 - z2));

               //x轴分量i
               float la = ((y1 - y2)*((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                           + (z1 - z2)*((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))) / a012 / l12;
               //y轴分量j
               float lb = -((x1 - x2)*((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                            - (z1 - z2)*((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1))) / a012 / l12;
               //z轴分量k
               float lc = -((x1 - x2)*((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))
                            + (y1 - y2)*((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1))) / a012 / l12;

               float ld2 = a012 / l12; // Eq. (2)//计算出距离

               // TODO: Why writing to a variable that's never read?
               pointProj = pointSel;
               pointProj.x -= la * ld2;
               pointProj.y -= lb * ld2;
               pointProj.z -= lc * ld2;

               //权重计算，距离越大权重越小，距离越小权重越大，得到的权重范围<=1
               float s = 1;
               if (iterCount >= 5)
               {//5次迭代之后开始增加权重因素
                  s = 1 - 1.8f * fabs(ld2);
               }

               //考虑权重
               coeff.x = s * la;
               coeff.y = s * lb;
               coeff.z = s * lc;
               coeff.intensity = s * ld2;

               if (s > 0.1 && ld2 != 0)
               {//只保留权重大的，也即距离比较小的点，同时也舍弃距离为零的
                  _laserCloudOri->push_back(_cornerPointsSharp->points[i]);
                  _coeffSel->push_back(coeff);
               }
            }
         }

         //处理planar point
         //对本次接收到的曲率最小的点,从上次接收到的点云曲率比较小的点中找三点组成平面，
         //一个使用kd-tree查找，另外一个在同一线上查找满足要求的，第三个在不同线上查找满足要求的
         //与上面对edge point的处理类似
         for (int i = 0; i < surfPointsFlatNum; i++)
         {
            transformToStart(_surfPointsFlat->points[i], pointSel);

            if (iterCount % 5 == 0)
            {
               //kd-tree查找第一个最近点，pointSearchInd——最近点的序号，pointSearchSqDis——离最近点的距离
               _lastSurfaceKDTree.nearestKSearch(pointSel, 1, pointSearchInd, pointSearchSqDis);
               int closestPointInd = -1, minPointInd2 = -1, minPointInd3 = -1;
               if (pointSearchSqDis[0] < 25)
               {
                  closestPointInd = pointSearchInd[0];
                  int closestPointScan = int(_lastSurfaceCloud->points[closestPointInd].intensity);

                  float pointSqDis, minPointSqDis2 = 25, minPointSqDis3 = 25;
                  for (int j = closestPointInd + 1; j < surfPointsFlatNum; j++)
                  {//
                     if (int(_lastSurfaceCloud->points[j].intensity) > closestPointScan + 2.5)
                     {
                        break;
                     }

                     pointSqDis = calcSquaredDiff(_lastSurfaceCloud->points[j], pointSel);

                     if (int(_lastSurfaceCloud->points[j].intensity) <= closestPointScan)
                     {
                        if (pointSqDis < minPointSqDis2)
                        {
                           minPointSqDis2 = pointSqDis;
                           minPointInd2 = j;
                        }
                     }
                     else
                     {
                        if (pointSqDis < minPointSqDis3)
                        {
                           minPointSqDis3 = pointSqDis;
                           minPointInd3 = j;
                        }
                     }
                  }
                  for (int j = closestPointInd - 1; j >= 0; j--)
                  {
                     if (int(_lastSurfaceCloud->points[j].intensity) < closestPointScan - 2.5)
                     {
                        break;
                     }

                     pointSqDis = calcSquaredDiff(_lastSurfaceCloud->points[j], pointSel);

                     if (int(_lastSurfaceCloud->points[j].intensity) >= closestPointScan)
                     {
                        if (pointSqDis < minPointSqDis2)
                        {
                           minPointSqDis2 = pointSqDis;
                           minPointInd2 = j;
                        }
                     }
                     else
                     {
                        if (pointSqDis < minPointSqDis3)
                        {
                           minPointSqDis3 = pointSqDis;
                           minPointInd3 = j;
                        }
                     }
                  }
               }

               _pointSearchSurfInd1[i] = closestPointInd;//kd-tree最近距离点,-1表示未找到满足要求的点
               _pointSearchSurfInd2[i] = minPointInd2;//同一线号上的距离最近的点，-1表示未找到满足要求的点
               _pointSearchSurfInd3[i] = minPointInd3;//不同线号上的距离最近的点，-1表示未找到满足要求的点
            }

            //计算planar point到上一帧点云中与之最近的planar的距离
            //参考论文"ow-drift and real-time lidar odometry and mapping"公式3
            if (_pointSearchSurfInd2[i] >= 0 && _pointSearchSurfInd3[i] >= 0)
            {
               tripod1 = _lastSurfaceCloud->points[_pointSearchSurfInd1[i]];
               tripod2 = _lastSurfaceCloud->points[_pointSearchSurfInd2[i]];
               tripod3 = _lastSurfaceCloud->points[_pointSearchSurfInd3[i]];

               float pa = (tripod2.y - tripod1.y) * (tripod3.z - tripod1.z)
                  - (tripod3.y - tripod1.y) * (tripod2.z - tripod1.z);
               float pb = (tripod2.z - tripod1.z) * (tripod3.x - tripod1.x)
                  - (tripod3.z - tripod1.z) * (tripod2.x - tripod1.x);
               float pc = (tripod2.x - tripod1.x) * (tripod3.y - tripod1.y)
                  - (tripod3.x - tripod1.x) * (tripod2.y - tripod1.y);
               float pd = -(pa * tripod1.x + pb * tripod1.y + pc * tripod1.z);

               float ps = sqrt(pa * pa + pb * pb + pc * pc);
               pa /= ps;
               pb /= ps;
               pc /= ps;
               pd /= ps;

               float pd2 = pa * pointSel.x + pb * pointSel.y + pc * pointSel.z + pd; //Eq. (3)??

               // TODO: Why writing to a variable that's never read? Maybe it should be used afterwards?
               pointProj = pointSel;
               pointProj.x -= pa * pd2;
               pointProj.y -= pb * pd2;
               pointProj.z -= pc * pd2;

               //同理计算权重
               float s = 1;
               if (iterCount >= 5)
               {
                  s = 1 - 1.8f * fabs(pd2) / sqrt(calcPointDistance(pointSel));
               }

               coeff.x = s * pa;
               coeff.y = s * pb;
               coeff.z = s * pc;
               coeff.intensity = s * pd2;

               if (s > 0.1 && pd2 != 0)
               {
                  _laserCloudOri->push_back(_surfPointsFlat->points[i]);
                  _coeffSel->push_back(coeff);
               }
            }
         }

         int pointSelNum = _laserCloudOri->points.size();
         if (pointSelNum < 10)
         {//满足要求的特征点至少10个，特征匹配数量太少弃用此帧数据
            continue;
         }

         Eigen::Matrix<float, Eigen::Dynamic, 6> matA(pointSelNum, 6);
         Eigen::Matrix<float, 6, Eigen::Dynamic> matAt(6, pointSelNum);
         Eigen::Matrix<float, 6, 6> matAtA;
         Eigen::VectorXf matB(pointSelNum);
         Eigen::Matrix<float, 6, 1> matAtB;
         Eigen::Matrix<float, 6, 1> matX;

         //计算matA,matB矩阵
         for (int i = 0; i < pointSelNum; i++)
         {
            const pcl::PointXYZI& pointOri = _laserCloudOri->points[i];
            coeff = _coeffSel->points[i];

            float s = 1;

            float srx = sin(s * _transform.rot_x.rad());
            float crx = cos(s * _transform.rot_x.rad());
            float sry = sin(s * _transform.rot_y.rad());
            float cry = cos(s * _transform.rot_y.rad());
            float srz = sin(s * _transform.rot_z.rad());
            float crz = cos(s * _transform.rot_z.rad());
            float tx = s * _transform.pos.x();
            float ty = s * _transform.pos.y();
            float tz = s * _transform.pos.z();

            float arx = (-s * crx*sry*srz*pointOri.x + s * crx*crz*sry*pointOri.y + s * srx*sry*pointOri.z
                         + s * tx*crx*sry*srz - s * ty*crx*crz*sry - s * tz*srx*sry) * coeff.x
               + (s*srx*srz*pointOri.x - s * crz*srx*pointOri.y + s * crx*pointOri.z
                  + s * ty*crz*srx - s * tz*crx - s * tx*srx*srz) * coeff.y
               + (s*crx*cry*srz*pointOri.x - s * crx*cry*crz*pointOri.y - s * cry*srx*pointOri.z
                  + s * tz*cry*srx + s * ty*crx*cry*crz - s * tx*crx*cry*srz) * coeff.z;

            float ary = ((-s * crz*sry - s * cry*srx*srz)*pointOri.x
                         + (s*cry*crz*srx - s * sry*srz)*pointOri.y - s * crx*cry*pointOri.z
                         + tx * (s*crz*sry + s * cry*srx*srz) + ty * (s*sry*srz - s * cry*crz*srx)
                         + s * tz*crx*cry) * coeff.x
               + ((s*cry*crz - s * srx*sry*srz)*pointOri.x
                  + (s*cry*srz + s * crz*srx*sry)*pointOri.y - s * crx*sry*pointOri.z
                  + s * tz*crx*sry - ty * (s*cry*srz + s * crz*srx*sry)
                  - tx * (s*cry*crz - s * srx*sry*srz)) * coeff.z;

            float arz = ((-s * cry*srz - s * crz*srx*sry)*pointOri.x + (s*cry*crz - s * srx*sry*srz)*pointOri.y
                         + tx * (s*cry*srz + s * crz*srx*sry) - ty * (s*cry*crz - s * srx*sry*srz)) * coeff.x
               + (-s * crx*crz*pointOri.x - s * crx*srz*pointOri.y
                  + s * ty*crx*srz + s * tx*crx*crz) * coeff.y
               + ((s*cry*crz*srx - s * sry*srz)*pointOri.x + (s*crz*sry + s * cry*srx*srz)*pointOri.y
                  + tx * (s*sry*srz - s * cry*crz*srx) - ty * (s*crz*sry + s * cry*srx*srz)) * coeff.z;

            float atx = -s * (cry*crz - srx * sry*srz) * coeff.x + s * crx*srz * coeff.y
               - s * (crz*sry + cry * srx*srz) * coeff.z;

            float aty = -s * (cry*srz + crz * srx*sry) * coeff.x - s * crx*crz * coeff.y
               - s * (sry*srz - cry * crz*srx) * coeff.z;

            float atz = s * crx*sry * coeff.x - s * srx * coeff.y - s * crx*cry * coeff.z;

            float d2 = coeff.intensity;

            matA(i, 0) = arx;
            matA(i, 1) = ary;
            matA(i, 2) = arz;
            matA(i, 3) = atx;
            matA(i, 4) = aty;
            matA(i, 5) = atz;
            matB(i, 0) = -0.05 * d2;
         }
         matAt = matA.transpose();
         matAtA = matAt * matA;
         matAtB = matAt * matB;

         //求解matAtA * matX = matAtB
         matX = matAtA.colPivHouseholderQr().solve(matAtB);

         if (iterCount == 0)
         {
            Eigen::Matrix<float, 1, 6> matE;//特征值1*6矩阵
            Eigen::Matrix<float, 6, 6> matV;//特征向量6*6矩阵
            Eigen::Matrix<float, 6, 6> matV2;

            //求解特征值/特征向量
            Eigen::SelfAdjointEigenSolver< Eigen::Matrix<float, 6, 6> > esolver(matAtA);
            matE = esolver.eigenvalues().real();
            matV = esolver.eigenvectors().real();

            matV2 = matV;

            isDegenerate = false;
            float eignThre[6] = { 10, 10, 10, 10, 10, 10 };//特征值取值门槛
            for (int i = 0; i < 6; i++)
            {//从小到大查找
               if (matE(0, i) < eignThre[i])
               {//特征值太小，则认为处在兼并环境中，发生了退化
                  for (int j = 0; j < 6; j++)
                  {
                     matV2(i, j) = 0;//对应的特征向量置为0
                  }
                  isDegenerate = true;
               }
               else
               {
                  break;
               }
            }
            matP = matV.inverse() * matV2;//计算P矩阵
         }

         if (isDegenerate)
         {//如果发生退化，只使用预测矩阵P计算
            Eigen::Matrix<float, 6, 1> matX2(matX);
            matX = matP * matX2;
         }

         //累加每次迭代的旋转平移量
         _transform.rot_x = _transform.rot_x.rad() + matX(0, 0);
         _transform.rot_y = _transform.rot_y.rad() + matX(1, 0);
         _transform.rot_z = _transform.rot_z.rad() + matX(2, 0);
         _transform.pos.x() += matX(3, 0);
         _transform.pos.y() += matX(4, 0);
         _transform.pos.z() += matX(5, 0);

         //判断是否为有效数值
         if (!pcl_isfinite(_transform.rot_x.rad()))
            _transform.rot_x = Angle();
         if (!pcl_isfinite(_transform.rot_y.rad()))
            _transform.rot_y = Angle();
         if (!pcl_isfinite(_transform.rot_z.rad()))
            _transform.rot_z = Angle();
         if (!pcl_isfinite(_transform.pos.x()))
            _transform.pos.x() = 0.0;
         if (!pcl_isfinite(_transform.pos.y()))
            _transform.pos.y() = 0.0;
         if (!pcl_isfinite(_transform.pos.z()))
            _transform.pos.z() = 0.0;

         //计算旋转平移量
         float deltaR = sqrt(pow(rad2deg(matX(0, 0)), 2) +
                             pow(rad2deg(matX(1, 0)), 2) +
                             pow(rad2deg(matX(2, 0)), 2));
         float deltaT = sqrt(pow(matX(3, 0) * 100, 2) +
                             pow(matX(4, 0) * 100, 2) +
                             pow(matX(5, 0) * 100, 2));

         if (deltaR < _deltaRAbort && deltaT < _deltaTAbort)
            break;//如果很小就停止迭代
      }//迭代结束
   }

   Angle rx, ry, rz;
   //求相对于原点的旋转量,垂直方向上1.05倍修正?
   accumulateRotation(_transformSum.rot_x,
                      _transformSum.rot_y,
                      _transformSum.rot_z,
                      -_transform.rot_x,
                      -_transform.rot_y.rad() * 1.05,
                      -_transform.rot_z,
                      rx, ry, rz);

   Vector3 v(_transform.pos.x() - _imuShiftFromStart.x(),
             _transform.pos.y() - _imuShiftFromStart.y(),
             _transform.pos.z() * 1.05 - _imuShiftFromStart.z());
   rotateZXY(v, rz, rx, ry);
   Vector3 trans = _transformSum.pos - v;

   //根据IMU修正旋转量
   pluginIMURotation(rx, ry, rz,
                     _imuPitchStart, _imuYawStart, _imuRollStart,
                     _imuPitchEnd, _imuYawEnd, _imuRollEnd,
                     rx, ry, rz);
   //得到世界坐标系下的转移矩阵
   _transformSum.rot_x = rx;
   _transformSum.rot_y = ry;
   _transformSum.rot_z = rz;
   _transformSum.pos = trans;

   //对点云的曲率比较大和比较小的点投影到扫描结束位置
   transformToEnd(_cornerPointsLessSharp);
   transformToEnd(_surfPointsLessFlat);

   _cornerPointsLessSharp.swap(_lastCornerCloud);
   _surfPointsLessFlat.swap(_lastSurfaceCloud);

   //畸变校正之后的点作为last点保存等下个点云进来进行匹配
   lastCornerCloudSize = _lastCornerCloud->points.size();
   lastSurfaceCloudSize = _lastSurfaceCloud->points.size();

   if (lastCornerCloudSize > 10 && lastSurfaceCloudSize > 100)
   {//点足够多就构建kd-tree，否则弃用此帧，沿用上一帧数据的kd-tree
      _lastCornerKDTree.setInputCloud(_lastCornerCloud);
      _lastSurfaceKDTree.setInputCloud(_lastSurfaceCloud);
   }

}



} // end namespace loam
