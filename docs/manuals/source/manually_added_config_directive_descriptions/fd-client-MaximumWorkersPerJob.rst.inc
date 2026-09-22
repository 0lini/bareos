.. note::
   The default is the number of available CPU cores on the system plus 4 (platform specific).
   Set this to ``0`` to disable multithreading.
   When PKI Encryption is not enabled, these worker threads are mostly used to compute checksums, and to compress & encrypt data.
   If this is set to at least 1, bareos will use a separate thread for sending data.
   When PKI Encryption is enabled, it disables Parallel Send code path, and all tasks happens in a single thread.
