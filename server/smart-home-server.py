#!/usr/bin/python

import discovery
import ota
import log_server

import time
import datetime
import threading
import time
import sys
import os
import md5
import paho.mqtt.client as mqtt
import pickle
from ConfigParser import ConfigParser

cfg = ConfigParser()
cfg.add_section('ota')
cfg.set('ota', 'filename', '../arduino/.pioenvs/nodemcuv2/firmware.bin')
cfg.add_section('sntp')
cfg.set('sntp', 'ip', '0.0.0.0')
cfg.add_section('mqtt')
cfg.set('mqtt', 'port', '1833')
cfg.set('mqtt', 'ip', '0.0.0.0')

def mqtt_connected(client, userdata, flags, rc):
    print('MQTT connected')

def mqtt_message(client, userdata, msg):
    print('MQTT message received')

def mqtt_published(client, userdata, msg):
    print('MQTT message published')

client = mqtt.Client()
client.on_connect = mqtt_connected
client.on_message = mqtt_message
client.on_publish = mqtt_published

def mqtt_topic(node_id, name):
    return "shm/%s/%s" % (node_id, name)

def mqtt_publish(node_id, name, value):
    print client.publish(mqtt_topic(node_id, name), value, 1, True)

def mqtt_run():
    server_thread = threading.Thread(target=client.loop_forever)
    server_thread.daemon = True
    server_thread.start()

class NodeList:
    DB_FILENAME = 'node_list.db'

    def __init__(self):
        self._index_by_ip = {}
        self._index_by_node = {}
        self.lock = threading.Lock()
        self.load_db()
        self._log = open('nodes.log', 'a', 4096)

    def load_db(self):
        try:
            f = file(self.DB_FILENAME, 'r')
            ip_index = pickle.load(f)
            node_index = pickle.load(f)
            f.close()
            self._index_by_ip = ip_index
            self._index_by_node = node_index
        except IOError:
            print 'Failed to load node list db'
        except EOFError:
            print 'Failed to load node list db (invalid file)'

    def save_db(self):
        try:
            f = file(self.DB_FILENAME, 'w')
            pickle.dump(self._index_by_ip, f)
            pickle.dump(self._index_by_node, f)
            f.close()
        except IOError:
            print 'Failed to write node list db'

    def get_node_by_ip(self, ip):
        if ip == '127.0.0.1': return {'node_id': 'local', 'node_type': 'test', 'node_desc': 'test'}
        with self.lock:
            node = self._index_by_ip.get(ip, None)
            if node is None:
                return None
            else:
                return self._index_by_node.get(node, None)

    def get_node_by_id(self, node_id):
        with self.lock:
            return self._index_by_node.get(node_id, None)

    def get_node_list(self):
        return map(lambda x: self._index_by_node[x].copy(), self._index_by_node)

    def update_node(self, node_ip, node_id, node_type, node_desc, version, static_ip, static_gw, static_nm, dns):
        with self.lock:
            print 'Updating node ip=%s id=%s type=%d desc=%s version=%s static_ip=%s gw=%s nm=%s dns=%s' % (node_ip, node_id, node_type, node_desc, version, static_ip, static_gw, static_nm, dns)
            old_node = self._index_by_node.get(node_id, None)
            node = {'node_id': node_id, 'node_type': node_type, 'node_desc': node_desc, 'version': version, 'last_seen_cpu': time.clock(), 'last_seen_wall': time.time(), 'ip': node_ip, 'static_ip': static_ip, 'static_gw': static_gw, 'static_nm': static_nm, 'dns': dns}
            if old_node:
                print 'Old node exists', len(old_node['node_desc']), len(node_desc)
                if node_type == 0:
                    node['node_type'] = old_node['node_type']
                if len(old_node['node_desc']) > 0 and len(node_desc) == 0:
                    print 'Rewriting fields'
                    for field in ('node_desc', 'static_ip', 'static_gw', 'static_nm', 'dns'):
                        old_val = old_node.get(field, '')
                        print 'Rewriting field', field, 'with old val', old_val, 'over', node[field]
                        node[field] = old_val
            self._index_by_node[node_id] = node
            self._index_by_ip[node_ip] = node_id
            self.save_db()
        mqtt_publish(node_id, "status", "discovery")
        mqtt_publish(node_id, "version", version)
        mqtt_publish(node_id, "desc", node_desc)
        return (node['node_desc'], node['static_ip'], node['static_gw'], node['static_nm'], node['dns'], node['node_type'])

    def upgrade(self, node_id, ota_file):
        node = self.get_node_by_id(node_id)
        if node is None:
            return None
        mqtt_publish(node_id, "upgrade", ota_file.get_version())
        return True

    def node_id_from_ip(self, ip):
        node = self.get_node_by_ip(ip)
        if node is None:
            return 'Unknown(%s)' % ip

        return node['node_id']

    def log(self, ip, data):
        ts = datetime.datetime.now().isoformat(' ')

        for line in data.splitlines():
            self._log.write('%s %s %s\n' % (self.node_id_from_ip(ip), ts, line))
        self._log.flush()

class OTAFile:
    def __init__(self, otaname):
        self._otaname = otaname
        self._content, self._md5, self._version = self._load_file()
        self.lock = threading.Lock()

    def get_version(self):
        self.update_file()
        return self._version

    def _load_file(self):
        content = file(self._otaname, 'r').read()
        content_md5 = md5.new(content).hexdigest()

        # Find the version in the image
        idx = content.find('SHMVER-')
        version = content[idx:]
        idx = version.find('\000')
        version = version[:idx]

        print('Loaded image, md5=%s version=%s len=%d' % (content_md5, version, len(content)))
        return content, content_md5, version

    def update_file(self):
        with self.lock:
            content, content_md5, version = self._load_file()
            if content_md5 != self._md5:
                print('File updated, new md5 is %s' % content_md5)
                self._md5 = content_md5
                self._content = content
                self._version = version

    def check_update(self, version):
        self.update_file()
        if version != self._version:
            return self._md5, self._content
        return None, None

def usage():
    print("USAGE: %s file_repository_folder" % sys.argv[0])
    exit(1)

def main():
    if len(sys.argv) != 2:
        usage()

    cfg.read(sys.argv[1])

    otaname = cfg.get('ota', 'filename')
    if not os.path.isfile(otaname):
        print 'ERROR: Path "%s" doesn\'t exist or is not a file\n' % otaname
        usage()

    print client.connect('127.0.0.1', 1883, 60)
    node_list = NodeList()
    otafile = OTAFile(otaname)

    server_ip = cfg.get('mqtt', 'ip')
    print 'Server IP is %s:%d' % (server_ip, cfg.getint('mqtt', 'port'))
    print 'SNTP server is', cfg.get('sntp', 'ip')
    mqtt_run()
    discovery.start(server_ip, cfg.getint('mqtt', 'port'), node_list, cfg.get('sntp', 'ip'))
    ota.start(node_list, otafile)
    log_server.start(node_list)
    while 1:
        time.sleep(1)

if __name__ == '__main__':
    main()
