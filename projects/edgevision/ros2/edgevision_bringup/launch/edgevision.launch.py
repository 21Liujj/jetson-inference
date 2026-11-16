from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    defaults = {
        'device': '/dev/v4l/by-id/usb-Generic_Web_Camera_20250708V1.000-video-index0',
        'model': '', 'labels': '', 'image_hz': '5.0', 'detection_hz': '5.0',
        'auto_start': 'true', 'publish_diagnostics': 'true',
    }
    return LaunchDescription([
        *[DeclareLaunchArgument(key, default_value=value) for key, value in defaults.items()],
        Node(package='edgevision_ros', executable='edgevision_node', name='edgevision',
             output='screen', parameters=[{key: LaunchConfiguration(key) for key in defaults},
                                           {'use_sim_time': False}]),
    ])
