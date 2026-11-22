# aember-fleet server: `docker build -t aember-fleet server/`, then
#   docker run --rm -v $PWD/fleet-data:/data aember-fleet init --name fleet.example
#   docker run -d -p 8443:8443 -v $PWD/fleet-data:/data aember-fleet
FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY pyproject.toml .
COPY aember_fleet aember_fleet
RUN pip install --no-cache-dir --no-deps .
ENV AEMBER_FLEET_DATA=/data
VOLUME /data
EXPOSE 8443
ENTRYPOINT ["aember-fleet"]
CMD ["serve"]
