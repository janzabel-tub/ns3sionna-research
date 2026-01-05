import matplotlib.pyplot as plt
import numpy as np
import sys
import os

print("\nPATH:       ", sys.argv[1], "\nOTHER PATH: ", sys.argv[2])

# load CSI
fname = sys.argv[1]
csi = np.asarray([complex(line.strip()) for line in open(fname, 'r')])

# Load baseline CSI with high channel calculations
fname_b = sys.argv[2]
csi_baseline = np.asarray([complex(line.strip()) for line in open(fname_b, 'r')])

print(len(csi_baseline))

print(len(csi))

if np.array_equal(csi, csi_baseline):
    print("IDENTICAL!")
else:
    print("NOT IDENTICAL!")

# reshape
NFFT = 3073

N = int(csi.shape[0] / NFFT)
all_csi = csi.reshape(N, NFFT)
print(all_csi)

N_b = int(csi_baseline.shape[0] / NFFT)
all_csi_b = csi_baseline.reshape(N_b, NFFT)
print(all_csi_b)

# Calculate Mean squared error (Accuracy?) between the two files.
mse = np.sqrt(np.mean(np.abs(all_csi - all_csi_b) ** 2))

# JZA: TODO: Calculate precision

print("Root Mean squared error: ", mse)
