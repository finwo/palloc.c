Freshly initialized dynamic does not need to reserve space, as it'll be dynamically allocated anyway







File structure:

- header
    - 4B header "PBA\0"
    - uint16_t  flags
- blobs
    - 8B free + size

- size indicator: data only, excludes size indicator itself

- free flag:
    - 1 = free
    - 0 = occupied

- blob structure:
    - free:
        - 8B size | flag
        - 8B pointer previous free block (0 = no previous free block)
        - 8B pointer next free block (0 = no next free block)
        - 8B size | flag
    - occupied:
        - 8B size
        - &lt;data[n]&gt;
        - 8B size
