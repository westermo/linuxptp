
import sys
import json



def get_all_field(data, field):
    ls = []
    for x in data:
        ls.append(x[field])
    return ls

def print_values(data, field):
    vals = get_all_field(data, field)
    print(f'Max {field} {max(vals)}')
    print(f'Min {field} {min(vals)}')
    print(f'Max-Min {field} {abs(max(vals) - min(vals))}')
    print(f'Mean {field} {sum(vals)//len(vals)}')

if len(sys.argv) < 2:
    print("Please specify input file!")
    exit(1)

lines = open(sys.argv[1]).readlines()

ports = {}

for line in lines:
    data = json.loads(line)
    name = data['name']
    if name in ports:
        ports[name].append(data)
    else:
        ports[name] = []
        ports[name].append(data)

for (port, data) in ports.items():
    print(f'===== {port} =====')
    print(f'Data points: {len(data)}')
    print()
    print_values(data, 'offset')
    print()
    print_values(data, 'path_delay')

