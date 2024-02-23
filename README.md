Freshly initialized dynamic does not need to reserve space, as it'll be dynamically allocated anyway







File structure:

- header
    - 4B header "PBA\0"
    - uint16_t  flags
- blobs
    - 8B occupied + size

- size indicator: data only, excludes size indicator itself

- occupied flag:
    - 0 = free
    - 1 = occupied

- blob structure:
    - occupied:
        - 8B size | flag
        - &lt;data[n]&gt;
        - 8B size | flag
    - free:
        - 8B size
        - 8B pointer previous free block
        - 8B pointer next free block
        - 8B size
