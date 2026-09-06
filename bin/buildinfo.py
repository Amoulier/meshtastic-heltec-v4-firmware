#!/usr/bin/env python3
import json
import sys

from readprops import readProps

verObj = readProps("version.properties")
propName = sys.argv[1]
print(json.dumps(verObj, sort_keys=True) if propName == "json" else verObj[propName])
