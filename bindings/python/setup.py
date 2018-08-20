 #
 # Copyright © 2018 IBM Corporation
 #
 # Licensed under the Apache License, Version 2.0 (the "License");
 # you may not use this file except in compliance with the License.
 # You may obtain a copy of the License at
 #
 #    http://www.apache.org/licenses/LICENSE-2.0
 #
 # Unless required by applicable law or agreed to in writing, software
 # distributed under the License is distributed on an "AS IS" BASIS,
 # WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 # See the License for the specific language governing permissions and
 # limitations under the License.
 #
from setuptools import setup, find_packages
import os, sys

setup(name='PyDBR',
      version='0.2',
      description='Python wrapper to the Data Broker C API',
      author='Claudia Misale',
      author_email='c.misale@ibm.com',
      packages=['dbr_module'],
      cffi_modules=["dbr_module/dbr_interface.py:ffibuilder","dbr_module/errorcodes.py:ffibuilder"],	
      package_data={
        # If any package contains *.txt files, include them:
        '': ['*.so','*.c','*.o'],
        # And include any *.dat files found in the 'data' subdirectory
        # of the 'mypkg' package, also:
        #'mypkg': ['data/*.dat'],
    }
)
