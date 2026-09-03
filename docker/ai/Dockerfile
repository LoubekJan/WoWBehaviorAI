# AI bridge (Etapa 1 / section 1.11). Health + a /decision stub that always
# returns action="NONE"; real inference logic is added in Etapa 2.
#
# Milestone 2.13A2 adds httpx (the /dynamic-task model provider's HTTP
# client) and bakes tests/ into the image so the unittest suite can be run
# against the exact built image via `docker compose run --rm ai-server
# python -m unittest ...` (see README_DEV.md) - production still only ever
# runs uvicorn, tests/ is just along for the ride.
FROM python:3.11-slim

WORKDIR /app

RUN pip install --no-cache-dir fastapi uvicorn httpx

COPY app/ ./app/
COPY tests/ ./tests/

EXPOSE 8000

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
