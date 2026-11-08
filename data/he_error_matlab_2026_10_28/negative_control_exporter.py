#!/usr/bin/python3
import json
import subprocess
import sys
subprocess.run(['/home/janzabel/Dokumente/ns3sionna_mimo/ns-allinone-3.40/ns-3.40/build/contrib/sionna/examples/ns3.40-ns3_he_error_reference_export-default',*sys.argv[1:]],check=True)
with open(sys.argv[2]) as stream:
    data=json.load(stream)
for case in data['cases']:
    if case['id']=='streams_mcs0_nss2_p2':
        case['effective']=0.001
with open(sys.argv[2],'w') as stream:
    json.dump(data,stream)
