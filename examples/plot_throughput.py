import matplotlib.pyplot as plt
import numpy as np
import sys
import os
import seaborn as sns
import csv

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

for j in range(100):
    fname = f"../../../tp_res/throughput_results_10_{i+1}.csv"
    try:
        with open(fname, newline='') as csvfile:
            spamreader = csv.reader(csvfile, delimiter=' ', quotechar='|')
            for row in spamreader:
                t_var = ', '.join(row).split(",")[1]
                if t_var != "Throughput_Mbps":
                    throughput_10.append(float(t_var))
    except FileNotFoundError:
        # print("File ", fname, " not found")
        continue

for j in range(100):
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

for j in range(100):
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


throughput = [throughput_1, throughput_1000]

# print(len(throughput[0]), len(throughput[1]))

sns.boxplot(data=throughput, widths=0.5)

plt.xlabel("Coherence Time in ms")
plt.ylabel("Mbps")

plt.xticks(ticks=[0, 1], labels=["0.1 ms", "100 ms"])
plt.ylim(15, 110)

plt.title("Average Throughput 100ms")

plt.savefig("throughput_boxplot.png")


