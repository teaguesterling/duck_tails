#!/usr/bin/env python3
"""
Example Python script for testing git integration.
"""


def calculate_sales_total(sales_data):
    """Calculate total sales from data."""
    total = 0
    for sale in sales_data:
        total += sale['amount']
    return total


def main():
    print("Duck Tails Git Integration Test")


if __name__ == "__main__":
    main()
