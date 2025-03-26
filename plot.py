import numpy as np
import matplotlib.pyplot as plt
import sys
from argparse import ArgumentParser

def scale(a):
    return a/1000000.0

parser = ArgumentParser(description="plot")

parser.add_argument('--dir', '-d',
                    help="Directory to store outputs",
                    required=True)

parser.add_argument('--name', '-n',
                    help="name of the experiment",
                    required=True)

parser.add_argument('--trace', '-tr',
                    help="name of the trace",
                    required=True)

parser.add_argument('--cwnd', '-c',
                    help="path to CWND.csv file",
                    default="CWND.csv")

args = parser.parse_args()

# Create two separate figures
# Figure 1: Throughput over time
fig1 = plt.figure(figsize=(21,3), facecolor='w')
ax1 = plt.gca()

# plotting the trace file
f1 = open (args.trace,"r")
BW = []
nextTime = 1000
cnt = 0
for line in f1:
    if int(line.strip()) > nextTime:
        BW.append(cnt*1492*8)
        cnt = 0
        nextTime+=1000
    else:
        cnt+=1
f1.close()

ax1.fill_between(range(len(BW)), 0, list(map(scale,BW)),color='#D3D3D3')

# plotting throughput
throughputDL = []
timeDL = []

traceDL = open (args.dir+"/"+str(args.name), 'r')
traceDL.readline()

tmp = traceDL.readline().strip().split(",")
bytes = int(tmp[1])
startTime = float(tmp[0])
stime=float(startTime)

for time in traceDL:
    if (float(time.strip().split(",")[0]) - float(startTime)) <= 1.0:
        bytes += int(time.strip().split(",")[1])
    else:
        throughputDL.append(bytes*8/1000000.0)
        timeDL.append(float(startTime)-stime)
        bytes = int(time.strip().split(",")[1])
        startTime += 1.0

print ("Throughput Data:")
print (timeDL)
print (throughputDL)

plt.plot(timeDL, throughputDL, lw=2, color='r')

plt.ylabel("Throughput (Mbps)")
plt.xlabel("Time (s)")
# plt.xlim([0,300])
plt.grid(True, which="both")
plt.savefig(args.dir+'/throughput.pdf', dpi=1000, bbox_inches='tight')

# Figure 2: CWND over time
try:
    fig2 = plt.figure(figsize=(21,3), facecolor='w')
    ax2 = plt.gca()
    
    # Read CWND data from CSV file
    cwnd_data = []
    cwnd_time = []
    
    with open(args.cwnd, 'r') as f:
        # Skip header line
        f.readline()
        
        for line in f:
            parts = line.strip().split(',')
            if len(parts) == 2:
                time_ms = float(parts[0])
                cwnd_value = float(parts[1])
                
                # Convert time to seconds
                cwnd_time.append(time_ms / 1000.0) 
                cwnd_data.append(cwnd_value)
    
    print("\nCWND Data:")
    print(cwnd_time)
    print(cwnd_data)
    
    plt.plot(cwnd_time, cwnd_data, lw=2, color='b')
    
    plt.ylabel("Congestion Window Size (packets)")
    plt.xlabel("Time (s)")
    plt.grid(True, which="both")
    plt.savefig(args.dir+'/cwnd.pdf', dpi=1000, bbox_inches='tight')
    print(f"CWND plot saved to {args.dir}/cwnd.pdf")
except Exception as e:
    print(f"Error plotting CWND data: {str(e)}")
