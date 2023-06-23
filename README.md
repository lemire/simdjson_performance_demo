# simdjson performance demonstration


This is a simple example where we seek to query a few key/value pairs from a sizeable
json document. We examine two different approaches.


Usage:

```
make
./main test.json
```

Optionally:

```
sudo ./main test.json
```

Sample result:

```
read_file                      :   0.96 GB/s   3.88 GHz    4.0 c/b  15.85 i/b   3.93 i/c 
two_pass_read_file             :   0.94 GB/s   3.88 GHz    4.1 c/b  16.77 i/b   4.06 i/c 
read_json                      :   3.49 GB/s   5.01 GHz    1.4 c/b   7.90 i/b   5.50 i/c 
fast_read_json                 :   4.93 GB/s   5.89 GHz    1.2 c/b   6.47 i/b   5.41 i/c 
file load + fast_read_json     :   0.89 GB/s   3.85 GHz    4.3 c/b  19.36 i/b   4.46 i/c 
```