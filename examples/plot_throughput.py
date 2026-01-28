import matplotlib.pyplot as plt
import numpy as np
import sys
import os
import seaborn as sns
import csv

# TODO: High mobility
# TODO: DONE Median unter Plot
# TODO: Munich outdoor
# TODO: 80Mhz Kanal
# TODO: IQR (Inter quantil distanzen) Was genau meinte ich damit?
# TODO: DONE Jupyter
# TODO: DONE Zeit messen
# TODO: Zeit plotten, durchschnitt im throughput?

throughput_1 = []
throughput_10 = []
throughput_100 = []
throughput_1000 = []

for i in range(100):
    fname = f"../../../tp_res/throughput_results_1_{i+1}.csv"
    try:
        with open(fname, newline='') as csvfile:
            spamreader = csv.reader(csvfile, delimiter=' ', quotechar='|')
            for row in spamreader:
                t_var = ', '.join(row).split(",")[1]
                if t_var != "Throughput_Mbps":
                    throughput_1.append(float(t_var))
    except FileNotFoundError:
        # print("File ", fname, " not found")
        continue

for i in range(100):
    fname = f"../../../tp_res/throughput_results_10_{i+1}.csv"
    try:
        with open(fname, newline='') as csvfile:
            spamreader = csv.reader(csvfile, delimiter=' ', quotechar='|')
            for row in spamreader:
                t_var = ', '.join(row).split(",")[1]
                if t_var != "Throughput_Mbps":
                    throughput_10.append(float(t_var))
    except FileNotFoundError:
        print("File ", fname, " not found")
        continue

for i in range(100):
    fname = f"../../../tp_res/throughput_results_100_{i+1}.csv"
    try:
        with open(fname, newline='') as csvfile:
            spamreader = csv.reader(csvfile, delimiter=' ', quotechar='|')
            for row in spamreader:
                t_var = ', '.join(row).split(",")[1]
                if t_var != "Throughput_Mbps":
                    throughput_100.append(float(t_var))
    except FileNotFoundError:
        # print("File ", fname, " not found")
        continue

for i in range(100):
    fname = f"../../../tp_res/throughput_results_1000_{i+1}.csv"
    try:
        with open(fname, newline='') as csvfile:
            spamreader = csv.reader(csvfile, delimiter=' ', quotechar='|')
            for row in spamreader:
                t_var = ', '.join(row).split(",")[1]
                if t_var != "Throughput_Mbps":
                    throughput_1000.append(float(t_var))
    except FileNotFoundError:
        print("File ", fname, " not found")
        continue


throughput = [throughput_1, throughput_10, throughput_100,throughput_1000]

# print(len(throughput[0]), len(throughput[1]))

box_plot = sns.boxplot(data=throughput, widths=0.5)

# Write medians onto the plot (slightly above the median line)
medians = [np.median(t) for t in throughput]

vertical_offset = [m * 0.01 for m in medians]

# print(medians)

# TODO: Check if white or black text looks better.
for xtick in box_plot.get_xticks():
    box_plot.text(
        xtick,
        medians[xtick] + vertical_offset[xtick],
        f'{medians[xtick]:.2f}',
        horizontalalignment='center',
        size='x-small',
        color='white',
        weight='semibold'
    )

plt.xlabel("Coherence Time in ms")
plt.ylabel("Mbps")

plt.xticks(ticks=[0, 1, 2, 3], labels=["0.1 ms", "1 ms", "10 ms", "100 ms"])
plt.ylim(0, 110)

plt.title("Throughput Boxplot, TCP Cubic, 20MHz channel, low mobility")

plt.savefig("throughput_boxplot.png")
