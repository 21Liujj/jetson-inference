import argparse
import json
import rclpy
from .ros_client import RosCoreClient
from .runner import AgentRunner


def main():
    parser=argparse.ArgumentParser(description='Local model chooses each real tool through the common ROS2 task runtime.')
    parser.add_argument('goal')
    parser.add_argument('--snapshot-root',required=True)
    parser.add_argument('--report-root',required=True)
    parser.add_argument('--request-id')
    parser.add_argument('--timeout',type=float,default=180)
    args=parser.parse_args();rclpy.init();client=RosCoreClient()
    try:
        result=AgentRunner(client,args.snapshot_root,args.report_root).run(args.goal,args.request_id,args.timeout)
        print(json.dumps(result,ensure_ascii=False,indent=2))
        raise SystemExit(0 if result['status']=='DONE' and not result.get('restore_error') else 1)
    finally:client.close();rclpy.shutdown()


if __name__=='__main__':main()
