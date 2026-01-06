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

csi_diff = np.abs(all_csi - all_csi_b)

# JZA: TODO: Calculate precision

print("Root Mean squared error: ", mse)

# plot mag squared
mag_csi = csi_diff ** 2

# create coordinate grid
x = np.arange(mag_csi.shape[1])
y = np.arange(mag_csi.shape[0])
X, Y = np.meshgrid(x, y)

# plot mag squared with pathloss
#mag_csi_pl = np.abs(all_csi * pathloss_lin[:, np.newaxis]) ** 2
#mag_csi_pl = (np.abs(all_csi) ** 2) * pathloss_lin[:, np.newaxis]

#mag_csi_pl_db = 10 * np.log10(mag_csi_pl)

fig = plt.figure()
ax = fig.add_subplot(121, projection='3d')

# Surface plot
surf = ax.plot_surface(X, Y, mag_csi, cmap='viridis')

# Labels and colorbar
ax.set_xlabel('OFDM subcarrier [idx]')
ax.set_ylabel('CSI sample [id]')
ax.set_zlabel('RMSE')
# fig.colorbar(surf, ax=ax, shrink=0.5, aspect=15)

ax.tick_params(axis='both', which='major', pad=5)

plt.subplots_adjust(left=0.1, right=0.9, top=0.9, bottom=0.1)

fig_name = "error.png"
plt.savefig(fig_name, dpi = 300, bbox_inches='tight', pad_inches=0.25)
print("Plot saved as: ", fig_name)

plt.close(fig)

#plt.tight_layout()
#plt.show()
