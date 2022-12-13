#!/usr/bin/env python

from setuptools import setup, Extension

version = "0.0"

lines = [x for x in open("max3100_module.c").read().split("\n") if "#define" in x and "_VERSION_" in x and "\"" in x]

if len(lines) > 0:
    version = lines[0].split("\"")[1]
else:
    raise Exception("Unable to find _VERSION_ in max3100_module.c")


classifiers = ['Development Status :: 5 - Production/Stable',
               'Operating System :: POSIX :: Linux',
               'License :: OSI Approved :: MIT License',
               'Intended Audience :: Developers',
               'Programming Language :: Python :: 2.6',
               'Programming Language :: Python :: 2.7',
               'Programming Language :: Python :: 3',
               'Topic :: Software Development',
               'Topic :: System :: Hardware',
               'Topic :: System :: Hardware :: Hardware Drivers']

setup(	name		= "max3100",
	version		= version,
	description	= "Python bindings for Linux serial access through MAX3100 using SPI and spidev",
	long_description= open('README.md').read() + "\n" + open('CHANGELOG.md').read(),
        long_description_content_type = "text/markdown",
	author		= "Nathan Edwards",
	author_email	= "edwardsnj@gmail.com",
	maintainer	= "Nathan Edwards",
	maintainer_email= "edwardsnj@gmail.com",
	license		= "MIT",
	classifiers	= classifiers,
	url		= "http://github.com/silver-sat/py-max3100",
	ext_modules	= [Extension("max3100", ["max3100_module.c"])]
)
