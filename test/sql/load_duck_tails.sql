-- Common SQL setup to load duck_tails extension
-- This file can be executed before running tests

-- Set the extension directory to the build output location
-- Note: This path needs to be adjusted based on your build location
SET extension_directory='__BUILD_DIRECTORY__/repository';

-- Load the duck_tails extension
LOAD duck_tails;

-- Optional: Set common test parameters
PRAGMA threads=1;