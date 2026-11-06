CREATE TABLE users (
  id     INT PRIMARY KEY AUTOINCREMENT,
  name   TEXT NOT NULL,
  email  TEXT UNIQUE,
  score  FLOAT,
  active BOOL DEFAULT TRUE
);
INSERT INTO users (name, email, score) VALUES ('Alice', 'alice@example.com', 3.5);
INSERT INTO users (name, email, score) VALUES ('Bob', 'bob@example.com', NULL);
INSERT INTO users (name, email, score, active) VALUES ('Chloé', 'chloe@example.com', 4.8, FALSE);
SELECT * FROM users ORDER BY score DESC;
