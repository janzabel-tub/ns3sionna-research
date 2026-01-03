import matplotlib.pyplot as plt
import numpy as np
import sys
import os

# load CSI
fname = sys.argv[1]
csi = np.asarray([complex(line.strip()) for line in open(fname, 'r')])

# Load baseline CSI with high channel calculations
fname_b = sys.argv[2]
csi_baseline = np.asarray([complex(line.strip()) for line in open(fname_b, 'r')])

# reshape
NFFT = 3073

N = int(csi.shape[0] / NFFT)
all_csi = csi.reshape(N, NFFT)

N_b = int(csi_baseline.shape[0] / NFFT)
all_csi_b = csi.reshape(N_b, NFFT)

# Calculate Mean squared error (Accuracy?) between the two files.
mse = np.mean(np.abs(all_csi - all_csi_b) ** 2)

# JZA: TODO: Calculate precision

print("Mean squared error: ", mse)
