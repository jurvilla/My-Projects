### Using DockerImageForFIREsmartDB

-- This is Assuming that you have correct table initialized in the Eureka DB, currently this script is assumes it will be sending data to the airwise_data table in the Eureka database
-- First specify mqtt and postgre connection variables by modifying the conectionSpecs.env file(the file is essentially a text file so just type in whatever the value is)
-- Secondly once inside the DockerImageForFIREsmartDB directory enter: $ docker compose build --no-cache, this rebuilds the docker image completely and sets it up for runtime
-- Next enter: $ docker compose up, the runs the docker image
-- Finally close the docker image by entering: $ docker compose down, this shuts down the docker image

### Building and running your application

When you're ready, start your application by running:
`docker compose up --build`.

Your application will be available at http://localhost:8000.

### Deploying your application to the cloud

First, build your image, e.g.: `docker build -t myapp .`.
If your cloud uses a different CPU architecture than your development
machine (e.g., you are on a Mac M1 and your cloud provider is amd64),
you'll want to build the image for that platform, e.g.:
`docker build --platform=linux/amd64 -t myapp .`.

Then, push it to your registry, e.g. `docker push myregistry.com/myapp`.

Consult Docker's [getting started](https://docs.docker.com/go/get-started-sharing/)
docs for more detail on building and pushing.

### References
* [Docker's Python guide](https://docs.docker.com/language/python/)
