#include "DiffDriverController.h"
#include <time.h>

namespace xqserial_server
{


DiffDriverController::DiffDriverController()
{
    max_wheelspeed=10;
    cmd_topic="/cmd_vel";
    xq_status=new StatusPublisher();
    cmd_serial_car=NULL;
    cmd_serial_imu=NULL;
    MoveFlag=true;
    left_speed_ = 0;
    right_speed_ = 0;
    linear_x_ = 0.;
    theta_z_ = 0.;
    send_flag_ = false;
    updateOrderflag_ = false;
    fastStopFlag_ = false;
    last_ordertime=ros::WallTime::now();
}

DiffDriverController::DiffDriverController(double max_speed_,std::string cmd_topic_,StatusPublisher* xq_status_,CallbackAsyncSerial* cmd_serial_car_,CallbackAsyncSerial* cmd_serial_imu_)
{
    MoveFlag=true;
    BarFlag=true;
    max_wheelspeed=max_speed_;
    cmd_topic=cmd_topic_;
    xq_status=xq_status_;
    cmd_serial_car=cmd_serial_car_;
    cmd_serial_imu=cmd_serial_imu_;
    left_speed_ = 0;
    right_speed_ = 0;
    linear_x_ = 0.;
    theta_z_ = 0.;
    send_flag_ = false;
    updateOrderflag_ = false;
    fastStopFlag_ = false;
    last_ordertime=ros::WallTime::now();
}

void DiffDriverController::run()
{
    ros::NodeHandle nodeHandler;
    ros::Subscriber sub = nodeHandler.subscribe(cmd_topic, 1, &DiffDriverController::dealCmd_vel, this);
    ros::Subscriber sub2 = nodeHandler.subscribe("/imu_cal", 1, &DiffDriverController::imuCalibration,this);
    ros::Subscriber sub3 = nodeHandler.subscribe("/global_move_flag", 1, &DiffDriverController::updateMoveFlag,this);
    ros::Subscriber sub4 = nodeHandler.subscribe("/barDetectFlag", 1, &DiffDriverController::updateBarDetectFlag,this);
    ros::Subscriber sub5 = nodeHandler.subscribe("/move_base/StatusFlag", 1, &DiffDriverController::updateFastStopFlag,this);
    ros::Rate r(100);//发布周期为50hz
    int i=0;
    while (ros::ok())
    {
      i++;
      ros::spinOnce();
      r.sleep();
      if (xq_status->get_status() == -1|| xq_status->get_status()>0) continue; //底层还在初始化
      ros::WallDuration t_diff = ros::WallTime::now() - last_ordertime;

      {
        boost::mutex::scoped_lock lock(mMutex);
        //3秒速度保持功能，
        if(t_diff.toSec()<3.0)
        {
          if(i%10==0 || updateOrderflag_)
          {
            updateOrderflag_ = false;

            //有速度指令则发速度，没有则停止
            if(send_flag_)
            {
              filterSpeed();
              send_speed();
            }
            else
            {
              send_stop();
            }
          }
          continue;
        }
        //3秒后开始锁轴
        fastStopFlag_ = false;
        send_flag_ = false;
      }//速度指令结束

    }
}

void DiffDriverController::updateMoveFlag(const std_msgs::Bool& moveFlag)
{
  boost::mutex::scoped_lock lock(mMutex);
  MoveFlag=moveFlag.data;
  //updateOrderflag_ = true;
  //last_ordertime=ros::WallTime::now();
}

void DiffDriverController::imuCalibration(const std_msgs::Bool& calFlag)
{
  if(calFlag.data)
  {
    //下发底层ｉｍｕ标定命令
    boost::mutex::scoped_lock lock(mMutex);
    char cmd_str[5]={(char)0xcd,(char)0xeb,(char)0xd7,(char)0x01,(char)0x43};
    if(NULL!=cmd_serial_imu)
    {
        cmd_serial_imu->write(cmd_str,5);
    }
  }
}

void DiffDriverController::updateBarDetectFlag(const std_msgs::Bool& DetectFlag)
{
  //避障传感器应急处理标志
  boost::mutex::scoped_lock lock(mMutex);
  BarFlag=DetectFlag.data;

}

void DiffDriverController::updateFastStopFlag(const std_msgs::Int32& fastStopmsg)
{
  boost::mutex::scoped_lock lock(mMutex);
  if(fastStopmsg.data == 2)
  {
    fastStopFlag_ = true;
    updateOrderflag_ = true;
    last_ordertime=ros::WallTime::now();
  }
  else
  {
    fastStopFlag_ = false;
  }
}

void DiffDriverController::dealCmd_vel(const geometry_msgs::Twist &command)
{
  boost::mutex::scoped_lock lock(mMutex);
  linear_x_ = command.linear.x ;
  theta_z_ = command.angular.z;
  send_flag_ = true;
  updateOrderflag_ = true;
  last_ordertime=ros::WallTime::now();
  //this->send_speed();
}

void DiffDriverController::filterSpeed()
{
  double vx_temp,vtheta_temp;
  vx_temp = linear_x_;
  vtheta_temp = theta_z_;

  if (!MoveFlag)
  {
    vx_temp = 0.;
    vtheta_temp = 0.;
  }

  if(BarFlag)
  {
    bool forward_flag,rot_flag;
    xq_status->get_canmove_flag(forward_flag,rot_flag);
    if(!forward_flag && vx_temp>0.01)
    {
      vx_temp = 0.;
    }
    if(!rot_flag)
    {
      vtheta_temp = 0.;
    }
  }

  linear_x_ = vx_temp;
  theta_z_ = vtheta_temp;
}

void DiffDriverController::send_speed()
{
  int i = 0;
  double separation = 0, radius = 0, speed_lin = 0, speed_ang = 0, speed_temp[2];

  separation = xq_status->get_wheel_separation();
  radius = xq_status->get_wheel_radius();
  //转换速度单位，由米转换成转
  speed_lin = linear_x_ / (2.0 * PI * radius);
  speed_ang = theta_z_ * separation / (2.0 * PI * radius);

  float scale = std::max(std::abs(speed_lin + speed_ang / 2.0), std::abs(speed_lin - speed_ang / 2.0)) / max_wheelspeed;
  if (scale > 1.0)
  {
    scale = 1.0 / scale;
  }
  else
  {
    scale = 1.0;
  }

  //转出最大速度百分比,并进行限幅
  speed_temp[0] = scale * (speed_lin + speed_ang / 2) / max_wheelspeed * 1000.0;
  speed_temp[0] = std::min(speed_temp[0], 1000.0);
  speed_temp[0] = std::max(-1000.0, speed_temp[0]);

  speed_temp[1] = scale * (speed_lin - speed_ang / 2) / max_wheelspeed * 1000.0;
  speed_temp[1] = std::min(speed_temp[1], 1000.0);
  speed_temp[1] = std::max(-1000.0, speed_temp[1]);

  left_speed_ = (int16_t)(speed_temp[1]);
  right_speed_ = -(int16_t)(speed_temp[0]);

  //下发速度指令
  char buffer [30];
  int len;
  len = std::sprintf(buffer, "!M %d  %d ", left_speed_,right_speed_);
  if(NULL!=cmd_serial_car)
  {
      cmd_serial_car->write(buffer,len);
  }
}

void DiffDriverController::send_stop()
{
  //下发速度指令
  char buffer [30];
  int len;
  len = std::sprintf(buffer, "!M %d  %d", 0,0);
  if(NULL!=cmd_serial_car)
  {
      cmd_serial_car->write(buffer,len);
  }
}

void DiffDriverController::send_fasterstop()
{
  //下发急停指令
  char buffer [30];
  int len;
  len = std::sprintf(buffer, "!EX");
  if(NULL!=cmd_serial_car)
  {
      cmd_serial_car->write(buffer,len);
  }
}

void DiffDriverController::send_release()
{
  //下发急停释放指令
  char buffer [30];
  int len;
  len = std::sprintf(buffer, "!MG");
  if(NULL!=cmd_serial_car)
  {
      cmd_serial_car->write(buffer,len);
  }
}





















}
