
from __future__ import absolute_import, print_function, unicode_literals
import _Framework.Capabilities as caps
from .MuseScore import MuseScore

def create_instance(c_instance):
    return MuseScore(c_instance)

def get_capabilities():
    return {caps.GENERIC_SCRIPT_KEY: True}