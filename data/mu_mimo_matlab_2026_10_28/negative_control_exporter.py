#!/usr/bin/python3
import json
import subprocess
import sys
result = subprocess.run(['/home/janzabel/Dokumente/ns3sionna_mimo/ns-allinone-3.40/ns-3.40/build/contrib/sionna/examples/ns3.40-ns3_mumimo_reference_export-default', *sys.argv[1:]], check=False)
if result.returncode:
    sys.exit(result.returncode)
with open(sys.argv[2]) as stream:
    data = json.load(stream)
data['cases'][0]['rows'][0][5] += 1.0
with open(sys.argv[2], 'w') as stream:
    json.dump(data, stream)
