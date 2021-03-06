#
# Copyright (C) 2011 Red Hat, Inc.
#
# Author: Angus Salkeld <asalkeld@redhat.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <http://www.gnu.org/licenses/>.
#
import os
import time
import logging
import libxml2
import exceptions
import uuid
import subprocess
from pwd import getpwnam

from glance import client as glance_client
from glance.common import exception
from glance.common import utils

from pcloudsh import pcmkconfig
from pcloudsh import assembly


class OpenstackAssembly(assembly.Assembly):
    def __init__(self, factory, name):
        assembly.Assembly.__init__(self, factory, name)
        os.system("modprobe nbd")
        self.l.debug('OpenstackAssembly creating %s' % name)
        if self.deployment == None:
            self.keydir = None
        else:
            self.keydir = os.path.join(self.conf.dbdir, self.deployment, 'novacreds')
        self.keyfile = 'nova_key'

    def start(self):
        self.l.debug('starting openstack:%s:%s' % (self.deployment, self.name))
        cmd = 'su -c \". ./novarc && euca-run-instances %s -k nova_key\" %s' % (self.name, self.username)
        self.l.debug('cmd: %s' % (str(cmd)))
        try:
            out = subprocess.check_output(cmd, shell=True, cwd=self.keydir)
            self.l.debug('cmd out: %s' % (str(out)))
        except:
            self.l.exception('*** couldn\'t start %s' % self.name)

    def image_to_instance(self, image_name):

        cmd = 'su -c \". ./novarc && euca-describe-images\" %s' % self.username
        output = subprocess.check_output(cmd, shell=True, cwd=self.keydir)

        image_name_search = ' (%s)' % image_name
        ami = None
        for line in output.split('\n'):
            if line != None and image_name_search in line:
                ami_s = line.split()
                if len(ami_s) > 1:
                    ami = ami_s[1]

        if ami is None:
            self.l.error('ami not found for %s', image_name)
            return None

        self.l.debug('image:%s => [ami:%s]' % (image_name, ami))

        cmd = 'su -c \". ./novarc && euca-describe-instances\" %s' % self.username
        output = subprocess.check_output(cmd, shell=True, cwd=self.keydir)
        inst = None
        for line in output.split('\n'):
            if line != None and ami in line:
                inst_s = line.split()
                if len(inst_s) > 0:
                    inst = inst_s[1]
                    self.l.debug('image:%s => [ami:%s, instance: %s]' % (image_name, ami, inst))
                    return inst

        self.l.error('instance not found for [image:%s, ami:%s]' % (image_name, ami))
        return None

    def stop(self):
        self.l.info('stopping %s:%s' % (self.deployment, self.name))

        inst = self.image_to_instance(self.name)
        if inst != None:
            try:
                cmd = 'su -c \". ./novarc && euca-terminate-instances %s\" %s' % (inst, self.username)
                self.l.debug('cmd: %s' % (str(cmd)))
                p1 = subprocess.Popen(cmd,
                    shell=True, cwd=self.keydir,
                    stderr=subprocess.PIPE, stdout=subprocess.PIPE)
                out = p1.communicate()
            except:
                self.l.exception('*** couldn\'t stop %s' % self.name)

    def status(self):
        inst = self.image_to_instance(self.name)
        if inst != None:
            return 'running'
        else:
            return 'stopped'

    def register_with_openstack(self, username):
        self.keydir = os.path.join(self.conf.dbdir, self.deployment, 'novacreds')
        print ' Registering assembly image with nova ...'

        creds = dict(username=os.getenv('OS_AUTH_USER'),
                password=os.getenv('OS_AUTH_KEY'),
                tenant=os.getenv('OS_AUTH_TENANT'),
                auth_url=os.getenv('OS_AUTH_URL'),
                strategy=os.getenv('OS_AUTH_STRATEGY', 'noauth'))

        c = glance_client.Client(host="0.0.0.0", port=9292,
                use_ssl=False, auth_tok=None,
                creds=creds)

        parameters = {
            "filters": {},
            "limit": 10,
        }

        image_meta = {'name': self.name,
                      'is_public': True,
                      'disk_format': 'qcow2',
                      'min_disk': 0,
                      'min_ram': 0,
                      'location': 'file://%s' % (self.image),
                      'owner': self.username,
                      'container_format': 'bare'}

        images = c.get_images(**parameters)
        for image in images:
            if image['name'] == self.name:
                print ' *** image already in glance: %s > %s' % (image['name'], image['id'])
                return

        try:
            with open(self.image) as ifile:
                image_meta = c.add_image(image_meta, ifile)
            image_id = image_meta['id']
            print " Added new image with ID: %s" % image_id
            print " Returned the following metadata for the new image:"
            for k, v in sorted(image_meta.items()):
                print " %(k)30s => %(v)s" % locals()
        except exception.ClientConnectionError, e:
            print (" Failed to connect to the Glance API server."
                   " Is the server running?" % locals())
            pieces = unicode(e).split('\n')
            for piece in pieces:
                print piece
            return
        except Exception, e:
            print " Failed to add image. Got error:"
            pieces = unicode(e).split('\n')
            for piece in pieces:
                print piece
            print (" Note: Your image metadata may still be in the registry, "
                   "but the image's status will likely be 'killed'.")

    def deregister_with_openstack(self):
        print ' deregistering assembly image from glance ...'

        parameters = {
            "filters": {},
            "limit": 10,
        }

        creds = dict(username=os.getenv('OS_AUTH_USER'),
                password=os.getenv('OS_AUTH_KEY'),
                tenant=os.getenv('OS_AUTH_TENANT'),
                auth_url=os.getenv('OS_AUTH_URL'),
                strategy=os.getenv('OS_AUTH_STRATEGY', 'noauth'))

        c = glance_client.Client(host="0.0.0.0", port=9292,
                                use_ssl=False, auth_tok=None,
                                creds=creds)

        images = c.get_images(**parameters)
        image_id = None
        for image in images:
            if image['name'] == self.name:
                image_id = image['id']
                break
        if image_id == None:
            print " *** No image with name %s was found in glance" % self.name
        else:
            try:
                c.delete_image(image_id)
                print " Deleted image %s (%s)" % (self.name, image_id)
            except exception.NotFound:
                print " *** No image with ID %s was found" % image_id
                return
            except exception.NotAuthorized:
                print " *** Glance can't delete the image (%s, %s)" % (image_id, self.name)
                return

