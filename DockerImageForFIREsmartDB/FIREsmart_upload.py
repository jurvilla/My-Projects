import paho.mqtt.client as mqtt
import json
import time
from datetime import datetime, timedelta
import os
import psycopg2
from psycopg2 import sql
import threading
import requests

# MQTT broker details
assert os.environ.get("mqtt_broker_address"), "no mqtt_broker_adress specified, check .env file"
broker_address = os.environ.get("mqtt_broker_address")
assert os.environ.get("mqtt_port"), "no mqtt_port specified, check .env file"
port = int(os.environ.get("mqtt_port"))
assert os.environ.get("mqtt_username"), "no mqtt_username specified, check .env file"
username = os.environ.get("mqtt_username")
assert os.environ.get("mqtt_password"), "no mqtt_password specified, check .env file"
password = os.environ.get("mqtt_password")
assert os.environ.get("pg_user"), "no pg_user specified, check .env file"
assert os.environ.get("pg_host"), "no pg_host specified, check .env file"
assert os.environ.get("pg_database"), "no pg_database specified, check .env file"
assert os.environ.get("pg_password"), "no pg_password specified, check .env file"
assert os.environ.get("pg_port"), "no pg_port specified, check .env file"
pg_options = {"user":os.environ.get("pg_user"), "host":os.environ.get("pg_host"), "database":os.environ.get("pg_database"), "password":os.environ.get("pg_password"), "port":int(os.environ.get("pg_port"))}

# Ntfy
NTFY_URL = "https://ntfy.sh/FIRESMART_Alerts"
OFFLINE_THRESHOLD_MINUTES = 150


# maps nodes
node_dict = {} 

# tracks last time we saw a message from each node
node_heartbeats = {}

#prevents multiple alerts for one offline node
node_alerts_sent = {}

# Topics
topics = [
    "msh/US/2/json/SensorData/!ba69aec8" 
    
]

def send_ntfy_alert(node_id, longname=None):
    try:
        if longname:
            message = f" Node OFFLINE: {longname} (**ID: {node_id}**) - No message for {OFFLINE_THRESHOLD_MINUTES} minutes"
        else:
            message = f" Node OFFLINE: **{node_id}** - No message for {OFFLINE_THRESHOLD_MINUTES} minutes"


        response = requests.post(NTFY_URL, data=message.encode('utf-8'), headers={"Title": "FIRESMART Node Alert", "Priority": "high", "Tags": "warning, rotating_light", "Markdown": "yes"})
        #could add an image in the message of where the node is located on the farm, once we place the nodes...

        if response.status_code == 200:
            print(f"Alert sent successfully for node {node_id}")
        else:
            print(f"Failed to send alert for node {node_id}: {response.status_code}")
            
    except Exception as e:
        print(f"Error sending ntfy alert: {e}")



# checks every minute for offline nodes
def check_node_heartbeats():
    while True:
        try:
            current_time = datetime.now()
            threshold = timedelta(minutes=OFFLINE_THRESHOLD_MINUTES)
            
            #check every node
            for node_id, last_seen in list(node_heartbeats.items()):
                time_since_last = current_time - last_seen
                
                # check if node is offline
                if time_since_last > threshold:
                    if not node_alerts_sent.get(node_id, False): # send one alert
                        node_info = node_dict.get(node_id, (None, None))
                        longname = node_info[1] if node_info else None
                        
                        print(f"Node {node_id} ({longname}) is OFFLINE - Last seen: {last_seen}")
                        send_ntfy_alert(node_id, longname)
                        node_alerts_sent[node_id] = True
                else:
                    # node is back online
                    if node_alerts_sent.get(node_id, False):
                        node_alerts_sent[node_id] = False
                        print(f"Node {node_id} is back ONLINE")
            
            
            time.sleep(600) # EVERY 10 MINUTES
            
        except Exception as e:
            print(f"Error in heartbeat checker: {e}")
            time.sleep(600)  

def parse_sensor_data(payload):
    try:
        data = json.loads(payload)
        
        node = data.get('from', None)

        # ignore packet if it is not telemetry
        if data.get('type', None) != "telemetry":
            print("Not telemetry packet, ignoring")
            return None
        
        payload_data = data.get('payload', {})

        # ignore packet if it is power telemetry (for now)
        if 'battery_level' in payload_data:
            print("Power telemetry packet, ignoring")
            return None


        # get node info from dictionary
        topic_info = node_dict.get(node, (None, None))


        return {
            'node': node,
            'topic_id': topic_info[0],
            'longname': topic_info[1],
            'pressure': payload_data.get('barometric_pressure', None),
            'gas': payload_data.get('gas_resistance', None),
            'iaq': payload_data.get('iaq', None),
            'humidity': payload_data.get('relative_humidity', None),
            'temperature': payload_data.get('temperature', None),
            'timestamp_node': data.get('timestamp', None),
            
            # add accurate time later datetime.utcnow()
        }
        
    except (json.JSONDecodeError, ValueError, KeyError) as e:
        print(f"Error parsing sensor data: {e}")
        return None

def insert_to_database(sensor_data):

    try:
        pg_client = psycopg2.connect(**pg_options)
        pg_cursor = pg_client.cursor()
        
        insert_query = """
            INSERT INTO airwise_data (node, topic_id, longname, pressure, gas, iaq, humidity, temperature, timestamp_node) 
            VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s)
        """

        pg_cursor.execute(insert_query, (
            sensor_data['node'],
            sensor_data['topic_id'],
            sensor_data['longname'],
            sensor_data['pressure'],
            sensor_data['gas'],
            sensor_data['iaq'],
            sensor_data['humidity'],
            sensor_data['temperature'],
            sensor_data['timestamp_node']
        ))
        
        pg_client.commit()
        print(f"Data successfully inserted into database - Node: {sensor_data['node']}, Temp: {sensor_data['temperature']}°C, Humidity: {sensor_data['humidity']}%")
        print()

        pg_cursor.close()
        pg_client.close()
        
    except psycopg2.Error as e:
        print(f"Database error: {e}")
        if 'pg_client' in locals():
            pg_client.rollback()
    except Exception as e:
        print(f"Unexpected error during database insertion: {e}")
    finally:
        # close connections 
        if 'pg_cursor' in locals() and pg_cursor:
            pg_cursor.close()
        if 'pg_client' in locals() and pg_client:
            pg_client.close()



# maps the unique "from" number to the topic id and longname using node_info packets
def map_nodes(payload):

    data = json.loads(payload)

    if data.get('type', None) != "nodeinfo":
        return None
    
    print("--------------------Node info packet received, mapping node------------------------------")
    
    payload_data = data.get('payload', {})
    topic_id = payload_data.get('id')
    longname = payload_data.get('longname')
    node = data.get('from', None)

    if node is not None and topic_id is not None and longname is not None:
        node_dict[node] = (topic_id, longname)
    
    # Update heartbeat timestamp
    if node is not None:
        node_heartbeats[node] = datetime.now()
        # reset alert flag if back online
        if node_alerts_sent.get(node, False):
            node_alerts_sent[node] = False
            print(f"Node {node} ({longname}) is back ONLINE - heartbeat received")



def on_connect(client, userdata, flags, reason_code, properties):
    print(f"Connected with result code {reason_code}")
    if reason_code == 0:
        print("Successfully connected to the broker.")
        for topic in topics:
            print(f"Subscribing to topic: {topic}")
            client.subscribe(topic)
    else:
        print(f"Failed to connect. Reason code: {reason_code}")

def on_message(client, userdata, msg):
    print(f"Received message on topic: {msg.topic}")
    try:
        #decode payload
        payload = msg.payload.decode()

        #maps nodes
        map_nodes(payload)


        print("Here is the payload: ", payload)
        
        #parse data
        sensor_data = parse_sensor_data(payload)

        print()
        print("Node mapping: ", node_dict)
        print()

        #insert data
        if sensor_data:
            insert_to_database(sensor_data)
        else:
            print("Failed to parse sensor data, skipping database insertion")
            print()
        
            
    except Exception as e:
        print(f"An unexpected error occurred while processing the message: {e}")

def on_disconnect(client, userdata, rc, properties=None, reason_code=None):
    if rc == 0:
        print("Disconnected successfully")
    else:
        print("Unexpected disconnection with result code:", rc)

# Test database connection on startup
def test_database_connection():
    try:
        pg_client = psycopg2.connect(**pg_options)
        pg_cursor = pg_client.cursor()
        pg_cursor.execute("SELECT version();")
        version = pg_cursor.fetchone()
        print(f"Successfully connected to PostgreSQL: {version[0]}")
        pg_cursor.close()
        pg_client.close()
        return True
    except Exception as e:
        print(f"Failed to connect to database: {e}")
        return False







# main loop

if __name__ == "__main__":
    if not test_database_connection():
        print("Exiting due to database connection failure")
        
        exit(1)



    # # --- TEST: pretend farm1 (!ba654d80, from=3127201152) went silent ---
    # node_dict[3127201152] = ("!ba654d80", "Farm1")          
    # node_heartbeats[3127201152] = datetime.now() - timedelta(minutes=10) 
    # node_alerts_sent.pop(3127201152, None)                  
    # # ------------------------------------------------------------------------------
    
    # Start the heartbeat checker thread
    heartbeat_thread = threading.Thread(target=check_node_heartbeats, daemon=True)
    heartbeat_thread.start()
    print("Started heartbeat monitoring thread")
    print(node_dict)
    
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.username_pw_set(username, password)

    client.on_connect = on_connect
    client.on_message = on_message
    client.on_disconnect = on_disconnect

    print(f"Connecting to {broker_address}:{port}")
    client.connect(broker_address, port, 60)


    # online alert- don't need if trying to get under max 250 messages per day
    requests.post(NTFY_URL, data=b"Monitor started: subscribed and listening.", headers={"Title":"FIRESMART Monitor Online","Priority":"default","Tags":"white_check_mark"})


    try:
        print("Starting the MQTT loop...")
        client.loop_forever()
    except KeyboardInterrupt:
        print("Script interrupted by user")
        client.disconnect()
    except Exception as e:
        print(f"An error occurred: {e}")
        client.disconnect()
