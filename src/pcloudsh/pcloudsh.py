#!/usr/bin/env python
#
# Copyright (C) 2011 Red Hat, Inc.
#
# Author: Steven Dake <sdake@redhat.com>
#         Angus Salkeld <asalkeld@redhat.com>
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
import string
import sys
import time
import random
import logging
import jeos
import assembly
import deployable
import cmd

class pcloudsh(cmd.Cmd, object):



    prompt = 'pcloudsh# '
    intro = "Welcome to pcloudsh, the Pacemaker Cloud Interface terminal\n\ntype \"help\" for help and quit to exit.\n"

    def __init__(self):
        self.j = jeos.Jeos()
        self.a = assembly.Assembly()
        self.d = deployable.Deployable()
        cmd.Cmd.__init__(self)

    def do_EOF(self, line):
        return True

    def preloop(self):
        return True

    def postloop(self):
        print "Shell completed!"

    def do_quit(self, s):
        return True

    def do_exit(self, s):
        return True

    def do_deployable_create(self, s):
        """
  deployable_create <deployable_name> - create a new deployable

  The deployable_create action creates a new deployable and stores it in the
  pcloudsh database.
        """
        options = s.split()
        if not len(options) == 1:
            self.do_help("deployable_create")
        else:
            self.d.create(options[0])

    def do_deployable_list(self, s):
        """
  deployable_list <deployable_list> - list all existing deployables

  The deployable_list action lists all deployables registered in the pcloudsh
  database.
        """
        deployable_names = []
        self.d.list(deployable_names)
        print 'Deployable names:'
        for deployable_name in deployable_names:
            print 'name:%s' % (deployable_name)

    def do_deployable_start(self, s):
        """
  deployable_start <deployable_name> - start a new deployable

  The deployable_start action starts a new deployable in the system
        """
        options = s.split()
        if not len(options) == 1:
            self.do_help("deployable_start")
        else:
            self.d.start(options[0])

    def do_deployable_assembly_add(self, s):
        """
  deployable_assembly_add <deployable_name> <assembly_name>
    - Add an assembly to a deployable

  The deployable_assembly_add action adds the assembly <assembly_name> to the
  deployable <deployable_name> and stores it in the pcloudsh database.
        """
        deployable_names = []
        options = s.split()
        if not len(options) == 2:
            self.do_help("deployable_assembly_add")
        else:
            self.d.assembly_add(options[0], options[1])

    def do_deployable_assembly_remove(self, s):
        """
  deployable_assembly_remove <deployable_name> <assembly_name>'
    - Remove an assembly from a deployable

  The deployable_assembly_remove action removes an existing assembly
  <assembly_name> from an existing deployable <deployable_name> and stores
  it in the pcloudsh database.
        """
        options = s.split()
        if not len(options) == 2:
            self.do_help("deployable_assembly_remove")
        else:
            self.d.assembly_remove(options[0], options[1])

    def do_assembly_clone(self, s):
        """
  assembly_clone <original_assembly> <new_assembly>'

  SYNOPSIS: Clones an assembly

  The assembly_clone action creates a copy of the original assembly.  Some
  post processing is done of the assembly image to ensure it will boot such
  as changing its mac address.  A new mac address is randomly generated
  for the clone.
        """
        options = s.split()
        if not len(options) == 2:
            self.do_help("assembly_clone")
        else:
            self.a.clone(options[1], options[0], "rhel61-x86_64")

    def do_assembly_create(self, s):
        """
  assembly_create <assembly_name> <F15|rhel61> <x86_64>'

  SYNOPSIS: Create an assembly

  The assembly_create action creates an assembly from the JEOS base image.
  Some post processing is done of the assembly image to ensure it will boot
  such as changing its mac address.  A new mac address is randomly generated
  for the new assembly.
        """
        options = s.split()
        if not len(options) == 3:
            self.do_help("assembly_create");
        elif not options[1] == 'F15' and not options[1] == 'rhel61':
            self.do_help("assembly_create");
            print 'supported platforms: F15, rhel61'
        elif not options[2] == 'x86_64':
            self.do_help("assembly_create");
        else:
            self.a.create(options[0], '%s-%s' % (options[1], options[2]))

    def do_assembly_delete(self, s):
        """
  assembly_delete <assembly_name>'

  SYNOPSIS: Delete an existing assembly

  The assembly_delete action deletes the named assembly.  No error checking is
  done to ensure it is not already part of an deployable.  Use this command with
  caution.
        """
        options = s.split()
        if not len(options) == 1:
            self.do_help("assembly_delete");
        else:
            self.a.delete(options[0])

    def do_assembly_list(self, s):
        """
  assembly_list

  SYNOPSIS: List the assemblies

  The assembly_list action lists all assemblies in the database.
        """
        options = s.split()
        if not len(options) == 0:
            self.do_help("assembly_delete");
        else:
            assembly_names = []
            self.a.list(assembly_names)
            print 'Assemblies:'
            for assembly_name in assembly_names:
                print 'name %s' % (assembly_name)

    def do_jeos_create(self, s):
        """
  jeos_create <F15|rhel61> <x86_64>'

  SYNOPSIS: Creates a JEOS image

  The jeos_create action creates a "(J)ust (E)nough (O)perating (S)ystem" image
  file.  This step should only be done once at initialization and can take up
  to 30 minutes to complete.  The current JEOS image types supported are either
  Fedora 15 or RHEL6.1.  The only supported architecture is x86_64.
        """

        options = s.split()
        if not len(options) == 2:
            self.do_help("jeos_create");
        elif not options[0] == 'F15' and not options[0] == 'rhel61':
            self.do_help("jeos_create");
        elif not options[1] == 'x86_64':
            self.do_help("jeos_create");
        else:
            try:
                print 'Creating a JEOS image - this can take up to 30 minutes'
                self.j.create(options[0], options[1])
            except:
                print 'JEOS image already present %s-%s' % (options[0], options[1])

    def do_jeos_list(self, s):
        """
  jeos_list

  SYNOPSIS: List JEOS images

  The jeos_list action lists existing JEOS images registered with the system.
        """
        jeos_names = []
        self.j.list(jeos_names)
        print 'JEOS images:'
        for jeos_name in jeos_names:
            print 'name %s' % (jeos_name)

if __name__ == '__main__':

    if len(sys.argv) > 1:
        pcloudsh().onecmd('%s' % string.join(sys.argv[1:]))
    else:
         pcloudsh().cmdloop()
