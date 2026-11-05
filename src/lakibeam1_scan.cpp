#include <rclcpp/rclcpp.hpp> 
#include <sensor_msgs/msg/laser_scan.hpp>

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>

#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <cmath>
#include <thread>
#include <atomic>
#include <vector>
#include <limits>
#include "../include/data_type.h"
#include "../include/remote.h"

#define DEG2RAD(x) ((x)*M_PI / 180.f)
using namespace std;

class lakibeam1_scan : public rclcpp::Node
{
public:
	lakibeam1_scan():Node("laser_scan_publisher")
	{
		declare_parameters();
		get_parameters();
		scan_pub = create_publisher<sensor_msgs::msg::LaserScan>(output_topic, 1000);
		info();
		// scan_config();
		scan_thread_ = std::thread(&lakibeam1_scan::scan_publish, this);
	}

	~lakibeam1_scan() override
	{
		is_running_ = false;
		if (sockfd != -1)
		{
			shutdown(sockfd, SHUT_RDWR);
			close(sockfd);
			sockfd = -1;
		}
		if (scan_thread_.joinable())
		{
			scan_thread_.join();
		}
	}

protected:
	void get_parameters()
	{
		get_parameter<string>("frame_id",frame_id);
		get_parameter<std::string>("port",port);
		get_parameter<string>("hostip",hostip);
		get_parameter<string>("sensorip",sensorip);
		get_parameter<string>("output_topic",output_topic);
		get_parameter<string>("scanfreq",scanfreq);
		get_parameter<string>("filter",filter);
		get_parameter<string>("laser_enable",laser_enable);
		get_parameter<string>("scan_range_start",scan_range_start);
		get_parameter<string>("scan_range_stop",scan_range_stop);
		get_parameter<bool>("inverted",inverted);
		get_parameter<int>("angle_offset",angle_offset);
		get_parameter<double>("socket_timeout_sec",socket_timeout_sec);
	}

	void declare_parameters()
	{
		declare_parameter<string>("frame_id","lakibeam_frame");
		declare_parameter<string>("port","2368");
		declare_parameter<string>("hostip","0.0.0.0");
		declare_parameter<string>("sensorip","192.168.3.4");
		declare_parameter<string>("output_topic","scan");
		declare_parameter<string>("scanfreq","30");
		declare_parameter<string>("filter","3");
		declare_parameter<string>("laser_enable","true");
		declare_parameter<string>("scan_range_start","45");
		declare_parameter<string>("scan_range_stop","315");
		declare_parameter<bool>("inverted",false);
		declare_parameter<int>("angle_offset",0);
		declare_parameter<double>("socket_timeout_sec",1.0);
	}

	void info()
	{
		RCLCPP_INFO(get_logger(),"frame_id:%s", frame_id.c_str());
		RCLCPP_INFO(get_logger(),"output_topic:%s", output_topic.c_str());
		RCLCPP_INFO(get_logger(),"inverted:%s", (inverted ? "True" : "False"));
		RCLCPP_INFO(get_logger(),"hostip:%s", hostip.c_str());
		RCLCPP_INFO(get_logger(),"sensorip:%s", sensorip.c_str());
		RCLCPP_INFO(get_logger(),"port:%s", port.c_str());
		RCLCPP_INFO(get_logger(),"scanfreq:%s", scanfreq.c_str());
		RCLCPP_INFO(get_logger(),"filter:%s", filter.c_str());
		RCLCPP_INFO(get_logger(),"laser_enable:%s", laser_enable.c_str());
		RCLCPP_INFO(get_logger(),"scan_range_start:%s", scan_range_start.c_str());
		RCLCPP_INFO(get_logger(),"scan_range_stop:%s", scan_range_stop.c_str());
		RCLCPP_INFO(get_logger(),"socket_timeout_sec:%.2f", socket_timeout_sec);
	}

	void scan_config()
	{
		RCLCPP_INFO(get_logger(),"scan_config");
		sensor_config(sensorip, "/api/v1/sensor/scanfreq", scanfreq);
		sensor_config(sensorip, "/api/v1/sensor/laser_enable", laser_enable);
		sensor_config(sensorip, "/api/v1/sensor/scan_range/start", scan_range_start);
		sensor_config(sensorip, "/api/v1/sensor/scan_range/stop", scan_range_stop);		
		RCLCPP_INFO(get_logger(),"scan_config1");
	}

	int create_socket()
	{
		if (sockfd != -1)
		{
			close(sockfd);
			sockfd = -1;
		}

		sockfd = socket(AF_INET, SOCK_DGRAM, 0);
		if(sockfd == -1)
		{
			RCLCPP_ERROR(get_logger(),"Failed to create socket: %s", strerror(errno));
			return -1;
		}

		int opt = 1;
		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
		{
			RCLCPP_WARN(get_logger(), "Failed to set SO_REUSEADDR: %s", strerror(errno));
		}

		struct timeval tv;
		tv.tv_sec = static_cast<time_t>(socket_timeout_sec);
		tv.tv_usec = static_cast<suseconds_t>((socket_timeout_sec - tv.tv_sec) * 1000000);
		if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
		{
			RCLCPP_WARN(get_logger(), "Failed to set SO_RCVTIMEO: %s", strerror(errno));
		}

		int rcvbuf_size = 2 * 1024 * 1024;
		if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) < 0)
		{
			RCLCPP_WARN(get_logger(), "Failed to set SO_RCVBUF: %s", strerror(errno));
		}

		memset(&ser_addr, 0, sizeof(ser_addr));
		ser_addr.sin_family = AF_INET;
		ser_addr.sin_addr.s_addr = inet_addr(hostip.c_str());
		ser_addr.sin_port = htons(atoi(port.c_str()));

		if(bind(sockfd, (struct sockaddr*)&ser_addr, sizeof(ser_addr)) < 0)
		{
			RCLCPP_ERROR(get_logger(),"Socket bind error on %s:%s: %s", hostip.c_str(), port.c_str(), strerror(errno));
			close(sockfd);
			sockfd = -1;
			return -1;
		}

		RCLCPP_INFO(get_logger(), "Socket bound successfully to %s:%s with timeout %.2fs",
					hostip.c_str(), port.c_str(), socket_timeout_sec);
		return 0;
	}

	void publish_scan()
	{
		if (scan_vec.empty())
		{
			return;
		}

		rclcpp::Time now = this->now();
		float duration = 0.0f;
		if (scan_begin.nanoseconds() > 0)
		{
			duration = static_cast<float>((now - scan_begin).seconds());
		}
		scan_end = scan_begin;
		scan_begin = now;

		if (duration <= 0.0f || duration > 1.0f)
		{
			duration = 1.0f / 30.0f;
		}

		sensor_msgs::msg::LaserScan scan;
		const uint16_t num_readings = scan_vec.size();
		scan.header.stamp = scan_begin;
		scan.header.frame_id = frame_id;
		scan.angle_min = DEG2RAD(-180 + angle_offset);
		scan.angle_max = DEG2RAD(180 + angle_offset);
		scan.angle_increment = 2.0 * M_PI / num_readings;
		scan.scan_time = duration;
		scan.time_increment = duration / static_cast<float>(num_readings) / 2.0f;
		scan.range_min = 0.0f;
		scan.range_max = 100.0f;
		scan.ranges.resize(num_readings);
		scan.intensities.resize(num_readings);

		const double inf = std::numeric_limits<double>::infinity();

		for (int idx = 0; idx < num_readings; idx++)
		{
			int target_idx = inverted ? (num_readings - idx - 1) : idx;
			float dist_m = static_cast<float>(scan_vec[idx].dist) / 1000.0f;
			uint8_t rssi = scan_vec[idx].rssi;

			if (dist_m <= 0.0f)
			{
				scan.ranges[target_idx] = inf;
				scan.intensities[target_idx] = 0;
			}
			else
			{
				scan.ranges[target_idx] = dist_m;
				scan.intensities[target_idx] = rssi;
			}
		}

		scan_pub->publish(scan);
		scan_vec.clear();
	}

	void scan_publish()
	{
		RCLCPP_INFO(get_logger(),"scan_publish thread started");

		uint16_t last_angle_raw = 0;

		while (rclcpp::ok() && is_running_)
		{
			if (sockfd == -1)
			{
				if (create_socket() < 0)
				{
					RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000,
						"Retrying socket bind to %s:%s in 1 second...", hostip.c_str(), port.c_str());
					std::this_thread::sleep_for(std::chrono::seconds(1));
					continue;
				}
			}

			socklen_t len = sizeof(clent_addr);
			ssize_t recv_len = recvfrom(sockfd, &MSOP_Data, sizeof(MSOP_Data), 0, (struct sockaddr*)&clent_addr, &len);

			if (recv_len < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
				{
					RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000,
						"LakiBeam packet receive timeout (%.1fs). Waiting for data from %s (port %s)...",
						socket_timeout_sec, sensorip.c_str(), port.c_str());
				}
				else if (errno == EINTR)
				{
					continue;
				}
				else
				{
					RCLCPP_ERROR_THROTTLE(get_logger(), *get_clock(), 2000,
						"recvfrom error: %s. Recreating socket...", strerror(errno));
					close(sockfd);
					sockfd = -1;
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
				}
				continue;
			}

			if (recv_len < static_cast<ssize_t>(sizeof(MSOP_Data)))
			{
				RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000,
					"Received incomplete packet size: %zd bytes (expected: %zu bytes)", recv_len, sizeof(MSOP_Data));
				continue;
			}

			if ((MSOP_Data.BlockID[1].Azimuth > MSOP_Data.BlockID[0].Azimuth))
			{
				int diff = MSOP_Data.BlockID[1].Azimuth - MSOP_Data.BlockID[0].Azimuth;
				if (diff > 0)
				{
					resolution = diff / 16;
					if (resolution <= 0)
					{
						resolution = 25;
					}
				}
			}

			for (int b = 0; b < 12; b++)
			{
				if (MSOP_Data.BlockID[b].DataFlag != 0xEEFF)
				{
					continue;
				}

				for (int k = 0; k < 16; k++)
				{
					uint16_t current_angle = MSOP_Data.BlockID[b].Azimuth + (resolution * k);

					// 1周（ラップアラウンド）判定: 360度境界越え、または angle 0 の検出
					bool is_wrap_around = false;
					if (!scan_vec.empty() && scan_vec.size() >= 200)
					{
						if (last_angle_raw > 18000 && current_angle < 18000)
						{
							is_wrap_around = true;
						}
						else if (current_angle == 0)
						{
							is_wrap_around = true;
						}
					}

					if (is_wrap_around)
					{
						publish_scan();
					}

					last_angle_raw = current_angle;

					bm_response_scan_t response_ptr;
					response_ptr.angle = current_angle;
					response_ptr.dist = MSOP_Data.BlockID[b].Result[k].Dist_1;
					response_ptr.rssi = MSOP_Data.BlockID[b].Result[k].RSSI_1;
					scan_vec.push_back(response_ptr);

					// 異常蓄積防止ガード（3000点超えで強制パブリッシュ）
					if (scan_vec.size() > 3000)
					{
						RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000,
							"Scan buffer exceeded 3000 points without lap completion. Forcing publish.");
						publish_scan();
					}
				}
			}
		}

		if (sockfd != -1)
		{
			close(sockfd);
			sockfd = -1;
		}
		RCLCPP_INFO(get_logger(), "scan_publish thread stopped");
	}

private:
	string hostip, sensorip, port, frame_id, output_topic, scanfreq, filter, laser_enable, scan_range_start, scan_range_stop;
	int resolution = 25, angle_offset = 0;
	double socket_timeout_sec = 1.0;
	bool inverted = false;
	rclcpp::Publisher<sensor_msgs::msg::LaserScan>::SharedPtr scan_pub;
	rclcpp::Time scan_begin, scan_end;
	struct sockaddr_in ser_addr, clent_addr;
	int sockfd = -1;
	std::vector<bm_response_scan_t> scan_vec;
	std::thread scan_thread_;
	std::atomic<bool> is_running_{true};
};

int main(int argc, char **argv)
{
	rclcpp::init(argc, argv);
	auto node = make_shared<lakibeam1_scan>();
	rclcpp::spin(node);
	rclcpp::shutdown();	

	return 0;
}
